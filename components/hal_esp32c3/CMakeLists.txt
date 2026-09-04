# 全屏蔽策略: mini_tree 的 ESP 构建 (cmake/esp_idf.cmake) 不编入任何 HAL weak stub,
# 本库提供 ESP 构建引用的全部 hal_* strong 实现 — 缺失即链接报错。
#   · 7 个外设 HAL: gpio/uart/spi/tim/adc/i2c/can (真实现, 对接 IDF 驱动)
#   · 4 个系统垫底: iwdg/storage/flash/usb (占位返回失败, 见各文件头注释)
# WHOLE_ARCHIVE: mini_tree 库的 .o 在链接行第二轮才被提取 (循环依赖重排), 产生的
# hal_* undefined 晚于本库常规扫描点。WHOLE_ARCHIVE 让 IDF 把本库追加到链接行
# 末尾再扫一次。去掉它 spi@0 会重新 "undefined reference to hal_spi_*"。
idf_component_register(
    WHOLE_ARCHIVE
    SRCS
        "src/hal_gpio_esp32c3.c"
        "src/hal_uart_esp32c3.c"
        "src/hal_spi_esp32c3.c"
        "src/hal_tim_esp32c3.c"
        "src/hal_adc_esp32c3.c"
        "src/hal_i2c_esp32c3.c"
        "src/hal_can_esp32c3.c"
        "src/hal_iwdg_esp32c3.c"
        "src/hal_storage_esp32c3.c"
        "src/hal_flash_esp32c3.c"
        "src/hal_usb_esp32c3.c"
        "src/hal_platform_safety_esp32c3.c"
    PRIV_INCLUDE_DIRS
        "${CMAKE_BINARY_DIR}/generated/board/mini_tree"
    REQUIRES
        h-000-h__mini-tree
        esp_driver_gpio
        esp_driver_uart
        esp_driver_spi
        esp_driver_i2c
        esp_driver_twai
        esp_driver_ledc
        esp_adc
        freertos
)
