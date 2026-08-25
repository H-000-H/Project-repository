# WHOLE_ARCHIVE: board_probe 对 DRIVER_REGISTER 是强引用, 但符号在独立
# 静态库中; 无 start-group 时仅靠依赖顺序可能扫不到本库. 整库链入, 替代 -u.
#
# 例外: esp_driver_rmt + led_strip 是本仓唯一允许的产品驱动厂商依赖
# （无 vfs-rmt）；其它 driver_* 不得再 REQUIRES esp_* / espressif/*。
idf_component_register(
    SRCS
        "src/ws2812_drv.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "${CMAKE_BINARY_DIR}/generated/board/mini_tree"
    REQUIRES
        mini_tree
        esp_driver_rmt
    WHOLE_ARCHIVE
)
