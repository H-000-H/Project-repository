cmake_minimum_required(VERSION 3.16)
project(ui_logic C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
# clangd 靠这份数据库跳转 (app/ui/.clangd、ui/.clangd、ui_pc/.clangd 指向它)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UI_STATIC "ON = 只编静态库(仅做编译检查), OFF = 动态库 .dll/.so" OFF)

# LVGL 源码编进本库, 用板级的 lv_conf.h
set(LV_BUILD_CONF_DIR ${ROOT}/ui/lvgl CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/mini_tree/lib/lvgl ${CMAKE_BINARY_DIR}/lvgl)

# 目标: 只要 app/ui 与 ui 两层 (+ app/secure 的常驻数据 + 可移植的 fifo 实现 + PC 适配桩)
file(GLOB_RECURSE UI_SRCS ${ROOT}/app/ui/*.cpp ${ROOT}/app/ui/*.c ${ROOT}/ui/*.cpp
                           ${ROOT}/app/secure/*/*.cpp)
file(GLOB BUF_SRCS ${ROOT}/mini_tree/algorithm/buffer/*.c)
# app/ui 下所有子目录都进 include: 新增页面目录不用回来改本文件
# (GLOB 会把同层文件如 .clangd 也列进来 → 只保留目录)
file(GLOB _APP_UI_DIRS_RAW ${ROOT}/app/ui/* ${ROOT}/app/ui/global/* ${ROOT}/app/ui/home/* ${ROOT}/app/secure/*)
set(_APP_UI_DIRS "")
foreach(_d IN LISTS _APP_UI_DIRS_RAW)
    if(IS_DIRECTORY ${_d})
        list(APPEND _APP_UI_DIRS ${_d})
    endif()
endforeach()
file(GLOB _HAL_ENTRIES ${ROOT}/mini_tree/hal/*) 
set(HAL_INC_DIRS "")
foreach(_e IN LISTS _HAL_ENTRIES)
    if(IS_DIRECTORY ${_e})
        list(APPEND HAL_INC_DIRS ${_e})
    endif()
endforeach()

if(UI_STATIC)
    set(UI_LIB STATIC)
else()
    set(UI_LIB SHARED)
endif()

add_library(ui_logic ${UI_LIB} ${UI_SRCS} ${BUF_SRCS} pc_port.cpp pc_display.cpp pc_input.cpp)

# PC 侧给 LVGL 一个大内存池: 320x240 底图的 layer/缓存远超 MCU 的 16KB,
# 池不够会分配失败返回空指针, LVGL 往里写就越界崩溃 (MCU 侧不覆盖, 保持 16KB)
target_compile_definitions(ui_logic PUBLIC LV_MEM_SIZE=8388608)  # 8MB, 别写成表达式: -D 里的括号会被命令行吃掉
target_compile_definitions(lvgl PRIVATE LV_MEM_SIZE=8388608)

add_executable(ui_host host_main.cpp)
target_link_libraries(ui_host PRIVATE ui_logic lvgl)

target_include_directories(ui_logic PUBLIC
    ${ROOT}/app/ui/main
    ${ROOT}/app/ui/indev
    ${_APP_UI_DIRS}
    ${ROOT}/app/button
    ${ROOT}/app/button_base
    ${ROOT}/ui
    ${ROOT}/ui/display
    ${ROOT}/ui/lvgl
    ${ROOT}/mini_tree/core/include
    ${ROOT}/mini_tree/board/include
    ${ROOT}/mini_tree/algorithm/buffer
    ${ROOT}/mini_tree/drivers/display/include
    ${ROOT}/mini_tree/vfs/gpio
    ${ROOT}/mini_tree/lib/mini-os/inc
    ${ROOT}/mini_tree/lib/etl/include
    ${ROOT}/mini_tree/mini-log/inc
    ${ROOT}/mini_tree/hal
    ${HAL_INC_DIRS}
    ${ROOT}/mini_tree/ide/stubs)

target_compile_definitions(ui_logic PUBLIC
    _Static_assert=static_assert
    ETL_NO_STL
    CONFIG_SYS_LOG_USE_MINI_LOG
    LV_CONF_INCLUDE_SIMPLE
    LV_KCONFIG_IGNORE
    LV_LVGL_H_INCLUDE_SIMPLE)

target_link_libraries(ui_logic PRIVATE lvgl)
