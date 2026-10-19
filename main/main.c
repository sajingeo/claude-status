/*
 * claude_status: show Claude Code's current state on the Waveshare
 * ESP32-S3-Touch-AMOLED-1.8 as an animated gif.
 *
 * A host-side Claude Code hook (host/cc-status.sh) writes one line per state
 * change to the board's USB-C serial port:
 *     STATE:idle  STATE:working  STATE:waiting  STATE:error
 *
 * Display per state:
 *   working - working.gif
 *   waiting - thinking.gif
 *   error   - error.gif
 *   idle    - rotates through bored.gif / dancing.gif / taking_a_stroll.gif
 * A short two-note chime plays on the working -> idle transition.
 *
 * GIFs are embedded as C arrays in main/gif_assets.c, generated from assets/
 * by tools/gen_gif_assets.py. Unknown serial lines (and ESP-IDF logs on the
 * same port) are ignored.
 */

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gif_assets.h"
#include "lvgl.h"

static const char *TAG = "claude_status";

#if LVGL_VERSION_MAJOR >= 9
#define ACTIVE_SCREEN()       lv_screen_active()
#define OBJ_REMOVE_FLAG(o, f) lv_obj_remove_flag((o), (f))
#else
#define ACTIVE_SCREEN()       lv_scr_act()
#define OBJ_REMOVE_FLAG(o, f) lv_obj_clear_flag((o), (f))
#endif

typedef enum {
    ST_IDLE = 0,
    ST_WORKING,
    ST_WAITING,
    ST_ERROR,
    ST_COUNT,
} cc_state_t;

typedef struct {
    const char *key;    /* token after "STATE:" on the wire */
    const char *word;   /* big label */
    uint32_t    color;  /* word accent */
} state_def_t;

/* Order must match cc_state_t. */
static const state_def_t STATES[ST_COUNT] = {
    [ST_IDLE]    = {"idle",    "IDLE",    0x4ADE80},  /* green */
    [ST_WORKING] = {"working", "WORKING", 0xF2A541},  /* amber */
    [ST_WAITING] = {"waiting", "WAITING", 0x5CC8D6},  /* cyan  */
    [ST_ERROR]   = {"error",   "ERROR",   0xEF4444},  /* red   */
};

/* idle cycles through these in order */
static const lv_image_dsc_t *IDLE_GIFS[] = {
    &gif_bored, &gif_dancing, &gif_taking_a_stroll,
};
#define IDLE_GIF_COUNT (sizeof(IDLE_GIFS) / sizeof(IDLE_GIFS[0]))
#define IDLE_ROTATE_MS (5 * 60 * 1000)   /* cycle idle gif every 5 minutes */

static lv_obj_t *s_gif;
static lv_obj_t *s_word;
static cc_state_t s_state = ST_IDLE;
static size_t s_idle_idx;

/* ---- audio: a short two-note chime on working -> idle ------------------- */
#define BEEP_SR 16000
static esp_codec_dev_handle_t s_spk;
static int16_t *s_beep;
static size_t   s_beep_samples;

static void beep_init(void)
{
    static const struct { float freq; int ms; } notes[] = { {784.0f, 90}, {1175.0f, 130} };
    const size_t fade = BEEP_SR * 4 / 1000;  /* 4 ms click-free ramp */
    size_t total = 0;
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        total += (size_t)BEEP_SR * notes[n].ms / 1000;
    }
    s_beep = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (s_beep == NULL) {
        ESP_LOGW(TAG, "beep buffer alloc failed");
        return;
    }
    size_t idx = 0;
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        size_t ns = (size_t)BEEP_SR * notes[n].ms / 1000;
        for (size_t i = 0; i < ns; i++, idx++) {
            float env = 1.0f;
            if (i < fade) {
                env = (float)i / (float)fade;
            } else if (i + fade > ns) {
                env = (float)(ns - i) / (float)fade;
            }
            float s = sinf(2.0f * 3.14159265f * notes[n].freq * (float)i / (float)BEEP_SR);
            s_beep[idx] = (int16_t)(s * env * 0.6f * 32767.0f);
        }
    }
    s_beep_samples = idx;
}

static void speaker_init(void)
{
    s_spk = bsp_audio_codec_speaker_init();   /* also enables the amp on GPIO46 */
    if (s_spk == NULL) {
        ESP_LOGW(TAG, "speaker init failed; continuing without audio");
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BEEP_SR,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ESP_LOGW(TAG, "codec open failed; continuing without audio");
        s_spk = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, 60);
    beep_init();
}

static void play_beep(void)
{
    if (s_spk != NULL && s_beep != NULL) {
        esp_codec_dev_write(s_spk, s_beep, (int)(s_beep_samples * sizeof(int16_t)));
    }
}

/* ---- UI ----------------------------------------------------------------- */
static const lv_image_dsc_t *gif_for(cc_state_t st)
{
    switch (st) {
    case ST_WORKING: return &gif_working;
    case ST_WAITING: return &gif_thinking;
    case ST_ERROR:   return &gif_error;
    default:         return IDLE_GIFS[s_idle_idx];  /* ST_IDLE */
    }
}

/* Caller holds the display lock (or runs in LVGL's own task). */
static void apply_state(cc_state_t st)
{
    const state_def_t *d = &STATES[st];
    lv_label_set_text(s_word, d->word);
    lv_obj_set_style_text_color(s_word, lv_color_hex(d->color), LV_PART_MAIN);
    lv_gif_set_src(s_gif, gif_for(st));
}

/* Rotates the idle gif; runs in LVGL's task, which holds the display lock. */
static void idle_tick(lv_timer_t *t)
{
    (void)t;
    if (s_state != ST_IDLE) {
        return;
    }
    s_idle_idx = (s_idle_idx + 1) % IDLE_GIF_COUNT;
    lv_gif_set_src(s_gif, IDLE_GIFS[s_idle_idx]);
}

static void build_ui(void)
{
    lv_obj_t *screen = ACTIVE_SCREEN();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    OBJ_REMOVE_FLAG(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Small state word at the top. */
    s_word = lv_label_create(screen);
    lv_obj_set_style_text_font(s_word, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_word, 2, LV_PART_MAIN);
    lv_obj_align(s_word, LV_ALIGN_TOP_MID, 0, 18);

    /* Big animated gif, scaled up from its native 128x128. Center pivot keeps
     * it centered as it grows. */
    s_gif = lv_gif_create(screen);
    lv_gif_set_color_format(s_gif, LV_COLOR_FORMAT_ARGB8888);
    lv_image_set_antialias(s_gif, true);
    lv_image_set_pivot(s_gif, 64, 64);
    lv_image_set_scale(s_gif, 720);        /* 256 = 100%; 720 -> ~360 px */
    lv_obj_align(s_gif, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "waiting for host " LV_SYMBOL_DOWNLOAD);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_timer_create(idle_tick, IDLE_ROTATE_MS, NULL);
}

/* ---- serial protocol ---------------------------------------------------- */
static bool parse_state(const char *line, cc_state_t *out)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "STATE:", 6) != 0) {
        return false;
    }
    p += 6;
    char key[16] = {0};
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && n < sizeof(key) - 1) {
        key[n] = (char)tolower((unsigned char)p[n]);
        n++;
    }
    for (int i = 0; i < ST_COUNT; i++) {
        if (strcmp(key, STATES[i].key) == 0) {
            *out = (cc_state_t)i;
            return true;
        }
    }
    ESP_LOGW(TAG, "unknown state: '%s'", key);
    return false;
}

static void set_state(cc_state_t st)
{
    if (st == s_state) {
        return;
    }
    cc_state_t prev = s_state;
    s_state = st;
    if (bsp_display_lock(100)) {
        apply_state(st);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "state -> %s", STATES[st].word);
    if (prev == ST_WORKING && st == ST_IDLE) {
        play_beep();   /* done chime */
    }
}

/* Reads STATE: lines from the USB-C port and drives the screen. */
static void serial_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    static char line[128];
    size_t len = 0;
    uint8_t buf[64];
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < got; i++) {
            char ch = (char)buf[i];
            if (ch == '\n' || ch == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    cc_state_t st;
                    if (parse_state(line, &st)) {
                        set_state(st);
                    }
                    len = 0;
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = ch;
            } else {
                len = 0;  /* overflow: drop the oversized line */
            }
        }
    }
}

void app_main(void)
{
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "display init failed: check I2C (expander 0x20, touch 0x15/0x38) above");
        abort();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));

    if (bsp_display_lock(1000)) {
        build_ui();
        apply_state(s_state);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "could not lock LVGL to build the UI");
    }

    speaker_init();   /* optional: firmware runs fine without audio */

    xTaskCreate(serial_task, "cc_serial", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready: send 'STATE:working' etc. on the USB-C serial port");
}
