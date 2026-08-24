cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_GENERATOR)
    set(CMAKE_GENERATOR "Ninja" CACHE INTERNAL "" FORCE)
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake"
        CACHE FILEPATH "Toolchain file")
endif()

#
# 平台自持构建（无 STM32CubeMX）：源文件列表由平台维护
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER arm-none-eabi-gcc) # 或者你的具体交叉编译器
set(CMAKE_CXX_COMPILER arm-none-eabi-g++) # 或者你的具体交叉编译器
    
# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME STM32F407ZGT6)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C CXX ASM)

# Create an executable object type (sources added by cmake/stm32 + app)
# 注：net/sys/* 已移除（lwIP 不再使用），源文件由 cmake/stm32 与各 target_sources 补充
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32 HAL/LL driver library + startup + app entry
add_subdirectory(cmake/stm32)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# mini_tree 中间件 — 配置见 mini_tree/.config
# 板级 DTS：平台最小骨架（业务节点后续补充），覆盖 mini_tree 占位板
set(BOARD_DTS "${CMAKE_CURRENT_SOURCE_DIR}/board/dts/board.dts" CACHE FILEPATH "板级 DTS (平台覆盖)")
set(BOARD_DTSI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/board/dtsi" CACHE PATH "板级 dtsi (平台覆盖)")
# dtc-lite 解析厂商宏：board/dtsi 中 #include <stm32f4xx_ll_*.h> 经此 -I/-D 展开
set(VENDOR_INC_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc"
    CACHE STRING "厂商 HAL 头搜索路径" FORCE)
set(VENDOR_DEFINES
    STM32F407xx
    USE_FULL_LL_DRIVER
    CACHE STRING "厂商设备宏" FORCE)
add_subdirectory(mini_tree)

# ---- USB：按 mini_tree/.config 的 CONFIG_USB 软编码（缺省启用，与中间件 Kconfig 一致）----
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/mini_tree/.config" CONFIG_USB_OFF REGEX "^# CONFIG_USB is not set$")

# ---- 平台 HAL：强符号覆盖 mini_tree weak 空实现 ----
file(GLOB PLATFORM_HAL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/hal/*/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/hal/*.c"
)
# mini_tree 自带同内容占位文件，避免重复编译
list(FILTER PLATFORM_HAL_SRCS EXCLUDE REGEX "hal_if_dummy\\.c$")
# USB 裁剪时板级 hal/usb glue（依赖 TinyUSB）一并剔除
if(CONFIG_USB_OFF)
    list(FILTER PLATFORM_HAL_SRCS EXCLUDE REGEX "/hal/usb/")
else()
    # 板级中断入口 (interrupt_stm32.c OTG_FS_IRQHandler) 按 CONFIG_USB 条件编译
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CONFIG_USB=1)
endif()
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLATFORM_HAL_SRCS})

file(GLOB PLATFORM_HAL_INC_DIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/hal/*/")
# GLOB 会把同层文件（hal_if_dummy.c / paths.cmake）也列进来 — 只保留目录
set(_PLATFORM_HAL_INC_DIRS "")
foreach(d ${PLATFORM_HAL_INC_DIRS})
    if(IS_DIRECTORY "${d}")
        list(APPEND _PLATFORM_HAL_INC_DIRS "${d}")
    endif()
endforeach()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/hal"
    ${_PLATFORM_HAL_INC_DIRS}
)

# ---- USB glue（usb_tusb_port / 描述符 / 网络回调 / DCD）— 按上面读出的 CONFIG_USB_OFF 门控 ----
if(NOT CONFIG_USB_OFF)
    # 契约头在中间件 bus/usb/usb_tusb_port.h（mini_tree INTERFACE include 已带）；板级只链实现
    # 中间件 v1.0+：TinyUSB 改为链接期惰性拉取（local-or-fetch），须显式调用 mini_tree_link_tinyusb
    # 创建 tinyusb 目标并链接；TINYUSB_SRC_DIR 在函数作用域内，从目标 include 属性取回源码目录
    mini_tree_link_tinyusb(${CMAKE_PROJECT_NAME})
    get_target_property(_tinyusb_inc_dirs tinyusb INTERFACE_INCLUDE_DIRECTORIES)
    list(GET _tinyusb_inc_dirs 0 TINYUSB_SRC_DIR)

    # TinyUSB DCD（板级驱动，中间件不绑 MCU）：STM32F407 OTG_FS = Synopsys DWC2
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        "${TINYUSB_SRC_DIR}/portable/synopsys/dwc2/dcd_dwc2.c"
        "${TINYUSB_SRC_DIR}/portable/synopsys/dwc2/dwc2_common.c"
    )
    # ECM/RNDIS 类源在 src/class/net，其 rndis_protocol.h 位于仓库 lib/networking
    # （TinyUSB 官方 tinyusb_target_add 会加此路径；mini_tree_link_tinyusb 未带，板级补齐）
    get_filename_component(_tinyusb_root "${TINYUSB_SRC_DIR}" DIRECTORY)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        "${TINYUSB_SRC_DIR}/portable/synopsys/dwc2"
        "${_tinyusb_root}/lib/networking"
    )
endif()

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries（与 mini_tree_link_tinyusb 的 PUBLIC 关键字签名保持一致）
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    stm32_platform
    mini_tree
)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/mini_tree/error_symbols.ld"
)

# ---- lwIP：按 mini_tree/.config 的 CONFIG_LWIP 软编码（缺省关闭，须显式开启才拉取/链接）----
# 与 USB 的 CONFIG_USB_OFF 门控对称：未开启 (CONFIG_LWIP is not set) 则不调用
# mini_tree_link_lwip，避免无条件触发 FetchContent 拉取 lwIP 源码。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/mini_tree/.config" CONFIG_LWIP_OFF REGEX "^# CONFIG_LWIP is not set$")
if(NOT CONFIG_LWIP_OFF)
    mini_tree_link_lwip(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/net")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/mini_tree/cmake/disasm.cmake")
mini_tree_add_disasm_postbuild(
    "${CMAKE_CURRENT_SOURCE_DIR}/mini_tree/.config"
    ${CMAKE_PROJECT_NAME}
)
