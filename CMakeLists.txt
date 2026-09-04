# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(ENV{IDF_PATH} "D:/Espressif_vscode/frameworks/esp-idf-v5.5.2")
set(IDF_TARGET esp32c3 CACHE STRING "ESP-IDF target" FORCE)
set(ENV{IDF_TARGET} "esp32c3")

# IDF 插件不加载 export.ps1 时补齐工具链 / Python（Windows 路径, 对照 esp32s3 工程）
set(IDF_TOOLCHAIN_DIR "D:/Espressif_vscode/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin")
set(IDF_PYTHON "D:/Espressif_vscode/python_env/idf5.5_py3.13_env/Scripts/python.exe")
set(ENV{IDF_PYTHON_ENV_PATH} "D:/Espressif_vscode/python_env/idf5.5_py3.13_env")
set(ENV{ESP_IDF_VERSION} "5.5.2")
set(ENV{PATH} "${IDF_TOOLCHAIN_DIR};$ENV{PATH}")
set(CMAKE_C_COMPILER   "${IDF_TOOLCHAIN_DIR}/riscv32-esp-elf-gcc.exe")
set(CMAKE_CXX_COMPILER "${IDF_TOOLCHAIN_DIR}/riscv32-esp-elf-g++.exe")
set(CMAKE_ASM_COMPILER "${IDF_TOOLCHAIN_DIR}/riscv32-esp-elf-gcc.exe")
list(PREPEND CMAKE_PROGRAM_PATH "${IDF_TOOLCHAIN_DIR}")
set(PYTHON "${IDF_PYTHON}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-c3)
