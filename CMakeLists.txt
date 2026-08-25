# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(ENV{IDF_PATH} "D:/Espressif_vscode/frameworks/esp-idf-v5.5.2")
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target" FORCE)
set(ENV{IDF_TARGET} "esp32s3")

# IDF 插件不加载 export.ps1 时补齐工具链 / Python（Windows 路径）
set(IDF_TOOLCHAIN_DIR "D:/Espressif_vscode/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin")
set(IDF_PYTHON "D:/Espressif_vscode/python_env/idf5.5_py3.11_env/Scripts/python.exe")
set(ENV{IDF_PYTHON_ENV_PATH} "D:/Espressif_vscode/python_env/idf5.5_py3.11_env")
set(ENV{ESP_IDF_VERSION} "5.5.2")
set(ENV{PATH} "${IDF_TOOLCHAIN_DIR};$ENV{PATH}")
set(CMAKE_C_COMPILER   "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-gcc.exe")
set(CMAKE_CXX_COMPILER "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-g++.exe")
set(CMAKE_ASM_COMPILER "${IDF_TOOLCHAIN_DIR}/xtensa-esp32s3-elf-gcc.exe")
list(PREPEND CMAKE_PROGRAM_PATH "${IDF_TOOLCHAIN_DIR}")
set(PYTHON "${IDF_PYTHON}")

# mini_tree 经组件注册表下载到 managed_components/，其 CMakeLists 不再能经
# 同级 ../board_port.cmake 发现板级注入；改用 MINI_TREE_BOARD_PORT 显式指向本工程
# 的 components/board_port.cmake（board DTS / 芯片 dtc args / ws2812 扫描）。
set(MINI_TREE_BOARD_PORT "${CMAKE_CURRENT_SOURCE_DIR}/components/board_port.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3)
