cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(APP_SRC 
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/led/led.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app/button/button.cpp
)

set(APP_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main
    ${CMAKE_CURRENT_SOURCE_DIR}/app/led
    ${CMAKE_CURRENT_SOURCE_DIR}/app/button
)

# Set the project name
set(CMAKE_PROJECT_NAME Host-Device-Architecture-stm32f103c8t6-node)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(BOARD_DTS "${CMAKE_CURRENT_SOURCE_DIR}/board/dts/board.dts" CACHE FILEPATH "板级 DTS" FORCE)
# 厂商 LL/CMSIS 头搜索路径：dtc-lite 预处理 dtsi 中 #include <stm32f1xx_ll_*.h> 时经此展开
set(VENDOR_INC_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F1xx/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Inc"
    CACHE STRING "厂商 HAL 头搜索路径" FORCE)
# 厂商设备宏：dtc-lite 预处理 dtsi 中 #include <stm32f1xx_ll_*.h> 时经此 -D 展开
# 缺此项会导致 STM32F103xB 未定义，stm32f1xx.h 中外设位宏被 #if 挡掉，LL 宏展开失败
set(VENDOR_DEFINES
    STM32F103xB
    USE_FULL_LL_DRIVER
    CACHE STRING "厂商设备宏" FORCE)
# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C CXX ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME}
)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
# Add mini_tree component framework
add_subdirectory(mini_tree)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${APP_SRC}
)

file(GLOB PLATFORM_HAL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/hal/*/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/hal/*.c"
)
# mini_tree 自带同内容占位文件，避免重复编译
list(FILTER PLATFORM_HAL_SRCS EXCLUDE REGEX "hal_if_dummy\\.c$")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PLATFORM_HAL_SRCS})

# Add include paths
file(GLOB PLATFORM_HAL_INC_DIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/hal/*/")
set(_PLATFORM_HAL_INC_DIRS "")
foreach(d ${PLATFORM_HAL_INC_DIRS})
    if(IS_DIRECTORY "${d}")
        list(APPEND _PLATFORM_HAL_INC_DIRS "${d}")
    endif()
endforeach()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${APP_INC}
    "${CMAKE_CURRENT_SOURCE_DIR}/hal"
    ${_PLATFORM_HAL_INC_DIRS}
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    mini_tree

    # Add user defined libraries
)
