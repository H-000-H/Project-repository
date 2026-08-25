# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(ENV{IDF_PATH} "/home/ning/esp-idf")
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target" FORCE)
set(ENV{IDF_TARGET} "esp32s3")

# IDF 插件不加载 export.sh 时补齐工具链 / Python
set(IDF_TOOLCHAIN_DIR "/home/ning/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin")
set(IDF_PYTHON "/home/ning/.espressif/tools/python/v6.0.1/venv/bin/python3")
set(ENV{IDF_PYTHON_ENV_PATH} "/home/ning/.espressif/tools/python/v6.0.1/venv")
set(ENV{ESP_IDF_VERSION} "6.0.1")
set(ENV{PATH} "${IDF_TOOLCHAIN_DIR}:$ENV{PATH}")
set(CMAKE_C_COMPILER   "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-gcc")
set(CMAKE_CXX_COMPILER "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-g++")
set(CMAKE_ASM_COMPILER "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-gcc")
list(PREPEND CMAKE_PROGRAM_PATH "${IDF_TOOLCHAIN_DIR}")
set(PYTHON "${IDF_PYTHON}")

# mini_tree 经组件注册表下载到 managed_components/，其 CMakeLists 不再能经
# 同级 ../board_port.cmake 发现板级注入；改用 MINI_TREE_BOARD_PORT 显式指向本工程
# 的 components/board_port.cmake（board DTS / 芯片 dtc args / ws2812 扫描）。
set(MINI_TREE_BOARD_PORT "${CMAKE_CURRENT_SOURCE_DIR}/components/board_port.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3)
