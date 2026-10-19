idf_component_register(
    SRCS "main.c" "gif_assets.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 esp_driver_usb_serial_jtag esp_codec_dev)

# Same warning relaxations Waveshare's examples use for LVGL with newer GCC.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=shift-count-negative)
idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
if(TARGET ${LVGL_LIB})
    target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes)
endif()
