cmake_minimum_required(VERSION 3.22)
enable_language(C CXX ASM)

# ============================================================
# cmake/stm32 — 平台驱动库（STM32 HAL/LL + startup + 应用入口）
# 无 STM32CubeMX：源文件列表由平台自持，不依赖 Cube 生成代码
# ============================================================

# Resolve to absolute paths so compile_commands.json / clangd do not rely on ../../
get_filename_component(STM32_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(STM32_DEFINES
    USE_FULL_LL_DRIVER
    USE_HAL_DRIVER
    STM32F407xx
    $<$<CONFIG:Debug>:DEBUG>
)

set(STM32_INCLUDE_DIRS
    "${STM32_ROOT}/main"
    "${STM32_ROOT}/app"
    "${STM32_ROOT}/app/stm-unique"
    "${STM32_ROOT}/app/led"
    "${STM32_ROOT}/app/ota"
    "${STM32_ROOT}/app/communicate"
    "${STM32_ROOT}/app/communicate/base"
    "${STM32_ROOT}/app/communicate/uart"
    "${STM32_ROOT}/app/cmd"
    "${STM32_ROOT}/app/cmd/uart"
    "${STM32_ROOT}/app/button"
    "${STM32_ROOT}/app/button/button_base"
    "${STM32_ROOT}/app/ui/main"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Inc"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy"
    "${STM32_ROOT}/Drivers/CMSIS/Device/ST/STM32F4xx/Include"
    "${STM32_ROOT}/Drivers/CMSIS/Include"
)

# 入口源按目标切换: boot 编 main_boot.cpp(bootloader 引导仲裁), app 编 main_app.cpp
# main_common.cpp(SystemClock_Config/Error_Handler) 为两个固件共用
if(BOOT_BUILD)
    set(STM32_APP_SRCS
        "${STM32_ROOT}/main/main_boot.cpp"
        "${STM32_ROOT}/main/main_common.cpp"
        "${STM32_ROOT}/startup_stm32f407xx.s"
    )
else()
    set(STM32_APP_SRCS
        "${STM32_ROOT}/main/main_app.cpp"
        "${STM32_ROOT}/main/main_common.cpp"
        "${STM32_ROOT}/startup_stm32f407xx.s"
        "${STM32_ROOT}/app/led/app_led.cpp"
        "${STM32_ROOT}/app/communicate/base/app_communicate_base.cpp"
        "${STM32_ROOT}/app/communicate/uart/app_uart_recv.cpp"
        "${STM32_ROOT}/app/ui/main/ui_task.cpp"
    )
endif()

set(STM32_Drivers_Src
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_gpio.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_adc.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_dac.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_dma.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_rcc.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_utils.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_exti.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_can.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_i2c.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_rtc.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_spi.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_tim.c"
    "${STM32_ROOT}/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_usart.c"
)

# STATIC — IDE/CMake 语言服务可正确看到源文件列表
add_library(STM32_Drivers STATIC ${STM32_Drivers_Src})
target_include_directories(STM32_Drivers PUBLIC ${STM32_INCLUDE_DIRS})
target_compile_definitions(STM32_Drivers PUBLIC ${STM32_DEFINES})

# 平台库别名：根 CMakeLists 经 target_link_libraries(... stm32_platform) 链接；
# 用 ALIAS 避免空 INTERFACE 触发 IDE "No SOURCES"
add_library(stm32_platform ALIAS STM32_Drivers)

# 若上层尚未创建可执行目标（例如根 CMakeLists 未 add_executable），此处自行创建，
# 避免强依赖 add_subdirectory 顺序；可执行源由本目录 target_sources 补充。
if(NOT TARGET "${CMAKE_PROJECT_NAME}")
    add_executable(${CMAKE_PROJECT_NAME})
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${STM32_APP_SRCS})
# keyword signature — 与根 CMakeLists target_link_libraries(... PRIVATE ...) 及 mini_tree_link_tinyusb 一致
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE STM32_Drivers)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    ADDITIONAL_CLEAN_FILES "${CMAKE_PROJECT_NAME}.map"
)

# IDE note: STM32_Drivers is STATIC with sources at add_library(); stm32_platform is ALIAS.
# If CMake Tools still shows stale "No SOURCES" on this file, run: CMake: Delete Cache and Reconfigure
