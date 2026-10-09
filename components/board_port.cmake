# SPDX-License-Identifier: Apache-2.0
# Board injection — answers "who is this project". Included by mini_tree/CMakeLists.txt (ESP_PLATFORM).
# Switching MCU = edit this file; or don't — point MINI_TREE_BOARD_PORT at another injection file.

# ── ① Path bases (fixed; no change when switching MCU) ──────────────────
# mini_tree 经组件注册表下载后位于 managed_components/，同级 ../board_port.cmake
# 发现机制失效，故由根 CMakeLists 以 MINI_TREE_BOARD_PORT 注入本文件；
# 此时 CMAKE_CURRENT_LIST_DIR 指向 mini_tree 目录，须用 CMAKE_SOURCE_DIR 定位工程根。
get_filename_component(_BOARD_ROOT "${CMAKE_SOURCE_DIR}/components" ABSOLUTE)   # components/
get_filename_component(_BOARD_DIR  "${_BOARD_ROOT}/board_esp32s3" ABSOLUTE) # board dts dir

# ── ② Board device tree ─────────────────────────────────────────────────
set(BOARD_DTS      "${_BOARD_DIR}/dts/board.dts")   # master dts (dtc-lite entry)
set(BOARD_DTSI_DIR "${_BOARD_DIR}/dtsi")            # dtsi fragment dir
file(GLOB MINI_TREE_BOARD_DTSI "${BOARD_DTSI_DIR}/*.dtsi")   # change re-runs dts

# ── ③ Chip-specific dtc args ────────────────────────────────────────────
# Base "-I mini_tree/board" (dt-bindings search) is provided by mini_tree by default;
# only chip-specific items go here.
if(DEFINED ENV{IDF_PATH})
    foreach(_d
        "$ENV{IDF_PATH}/components/esp_hal_gpio/include"
        "$ENV{IDF_PATH}/components/esp_hal_uart/include"
        "$ENV{IDF_PATH}/components/esp_hal_gpspi/include"
        "$ENV{IDF_PATH}/components/esp_hal_i2c/include"
        "$ENV{IDF_PATH}/components/esp_hal_twai/include"
        "$ENV{IDF_PATH}/components/soc/esp32s3/include"
        "$ENV{IDF_PATH}/components/soc/include"
        "$ENV{IDF_PATH}/components/esp_common/include"
        "$ENV{IDF_PATH}/components/hal/include"
        "$ENV{IDF_PATH}/components/hal/platform_port/include"
        "$ENV{IDF_PATH}/components/esp_hw_support/include"
        "$ENV{IDF_PATH}/components/esp_rom/include"
    )
        if(IS_DIRECTORY "${_d}")        # IDF layout changes between versions; add only if present (skip is fine)
            list(APPEND MINI_TREE_DTC_EXTRA_ARGS "-I${_d}")
        endif()
    endforeach()
    # Target macros: enable the #ifdef CONFIG_IDF_TARGET_<CHIP> branches inside dtsi
    list(APPEND MINI_TREE_DTC_EXTRA_ARGS
        "-DCONFIG_IDF_TARGET_ESP32S3=1"
        "-DIDF_TARGET_ESP32S3=1")
endif()

# ── ④ Out-of-tree product drivers ───────────────────────────────────────
file(GLOB _OUT_DRV_SRCS "${_BOARD_ROOT}/driver_ws2812/src/*.c")
set(MINI_TREE_DTC_EXTRA_SCAN_DIRS "${_BOARD_ROOT}/driver_ws2812/src")   # dtc scans DRIVER_REGISTER
set(MINI_TREE_DTC_EXTRA_DEPENDS  ${MINI_TREE_BOARD_DTSI} ${_OUT_DRV_SRCS} "${BOARD_DTS}")
