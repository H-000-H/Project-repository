# mini_tree_ui — UI 库胶水层 (LVGL / u8g2)。
# 只经 device/VFS 模型 (device_open/ioctl + DISPLAY_CMD_*) 触硬件, 不直调 bus_*/hal_*。
# 是否编入由 .config 的 CONFIG_UI_LVGL / CONFIG_UI_U8G2 桥接 (仿照仓库既有
# file(STRINGS KCONFIG_DOT ...) 范式), 与 Kconfig 默认值保持一致。

set(MINI_TREE_UI_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

set(MINI_TREE_UI_DOTCONFIG "${MINI_TREE_UI_ROOT_DIR}/.config")
if(EXISTS "${MINI_TREE_UI_DOTCONFIG}")
    file(STRINGS "${MINI_TREE_UI_DOTCONFIG}" _ui_lvgl_on REGEX "^CONFIG_UI_LVGL=y$")
    file(STRINGS "${MINI_TREE_UI_DOTCONFIG}" _ui_u8g2_on REGEX "^CONFIG_UI_U8G2=y$")
else()
    set(_ui_lvgl_on "")
    set(_ui_u8g2_on "")
endif()

set(MINI_TREE_UI_SRCS "")
if(_ui_lvgl_on)
    list(APPEND MINI_TREE_UI_SRCS
        lvgl/lv_conf.h
        lvgl/lvgl_port.h
        lvgl/lvgl_port.c
    )
endif()
if(_ui_u8g2_on)
    list(APPEND MINI_TREE_UI_SRCS
        u8g2/u8g2_port.h
        u8g2/u8g2_port.c
    )
endif()

add_library(mini_tree_ui STATIC
    display/display_ui_bridge.h
    touch/xpt2046_bridge.h
    touch/ft5x06_bridge.h
    ${MINI_TREE_UI_SRCS}
)

# mini_tree 的头路径在根 CMake 经 mini_tree PUBLIC 传播; 此处补齐胶水层
# 自身所需 (device/board、display 命令、触摸驱动、HAL 头、kconfig/dt 生成头)。
# HAL_INCLUDE_DIRS 由根 CMake 先 include 的 hal/paths.cmake 提供。
# 注: 生成目录在父工程构建时指向父工程 CMAKE_BINARY_DIR (与根 CMake 的
# GENERATED_BOARD_DIR / KCONFIG_GEN_DIR 同源)。
target_include_directories(mini_tree_ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/display"
    "${CMAKE_CURRENT_SOURCE_DIR}/touch"
    "${MINI_TREE_UI_ROOT_DIR}"
    "${MINI_TREE_UI_ROOT_DIR}/board/include"
    "${MINI_TREE_UI_ROOT_DIR}/core/include"
    "${MINI_TREE_UI_ROOT_DIR}/osal/include"
    "${MINI_TREE_UI_ROOT_DIR}/system_c/include"
    "${MINI_TREE_UI_ROOT_DIR}/drivers/display/include"
    "${MINI_TREE_UI_ROOT_DIR}/drivers/xpt2046/include"
    "${MINI_TREE_UI_ROOT_DIR}/drivers/ft5x06/include"
    ${HAL_INCLUDE_DIRS}
    "${CMAKE_BINARY_DIR}/generated/kconfig/mini_tree"
    "${CMAKE_BINARY_DIR}/generated/board/mini_tree"
)

if(_ui_lvgl_on)
    # lv_conf.h 位于 ui/lvgl/, 经 LV_BUILD_CONF_DIR 注入 lvgl 库与胶水层
    mini_tree_link_lvgl(mini_tree_ui "${CMAKE_CURRENT_SOURCE_DIR}/lvgl")
endif()
if(_ui_u8g2_on)
    mini_tree_link_u8g2(mini_tree_ui)
endif()
