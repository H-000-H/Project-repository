# =============================================================================
# hal/ — CH32V307 平台层
#   - ch32_periph : WCH 标准外设库 (STATIC, 厂商代码)
#   - ch32_hal    : mini_tree HAL 弱符号的强覆盖实现 (OBJECT)
#                   OBJECT 库保证对象文件无条件参与链接, 强符号覆盖
#                   libmini_tree.a 内的 COMPAT_WEAK 空桩 (STATIC 库则不会
#                   被链接器拉入, 覆盖失效)。
# =============================================================================

set(CH32_HAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# -----------------------------------------------------------------------------
# 1. WCH 标准外设库 (仅收录新 HAL 实现实际依赖的外设模块)
# -----------------------------------------------------------------------------
add_library(ch32_periph STATIC
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_gpio.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_rcc.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_usart.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_spi.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_i2c.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_tim.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_adc.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_dma.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_exti.c"
        "${CH32_HAL_ROOT}/SRC/Peripheral/src/ch32v30x_misc.c"
)
target_include_directories(ch32_periph PUBLIC
        "${CH32_HAL_ROOT}/SRC/Peripheral/inc"
        "${CH32_HAL_ROOT}/SRC/Core"
        "${CH32_HAL_ROOT}/SRC/Debug"
        "${CH32_HAL_ROOT}/SRC/User"
)

# -----------------------------------------------------------------------------
# 2. mini_tree HAL 强符号覆盖 (OBJECT 库, 对象必进链接)
# -----------------------------------------------------------------------------
add_library(ch32_hal OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_gpio_ch32v30x.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_uart_ch32v30x.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_spi_ch32v30x.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_i2c_ch32v30x.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_tim_ch32v30x.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hal_adc_ch32v30x.c"
)

set(MINI_TREE_DIR "${CH32_HAL_ROOT}/mini_tree")
target_include_directories(ch32_hal PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${MINI_TREE_DIR}/core/include"
        "${MINI_TREE_DIR}/algorithm/buffer"
        "${MINI_TREE_DIR}/board"
        "${MINI_TREE_DIR}/board/include"
        "${MINI_TREE_DIR}/hal/gpio"
        "${MINI_TREE_DIR}/hal/uart"
        "${MINI_TREE_DIR}/hal/spi"
        "${MINI_TREE_DIR}/hal/i2c"
        "${MINI_TREE_DIR}/hal/tim"
        "${MINI_TREE_DIR}/hal/adc"
        "${MINI_TREE_DIR}/interrupt"
        "${CH32_HAL_ROOT}/SRC/Peripheral/inc"
        "${CH32_HAL_ROOT}/SRC/Core"
        "${CH32_HAL_ROOT}/SRC/Debug"
        "${CH32_HAL_ROOT}/SRC/User"
        "${CMAKE_BINARY_DIR}/generated/kconfig/mini_tree"
)

# 自持架构编译选项 (hal/ 先于根目录编译选项区处理, 保证 ABI 一致)
target_compile_options(ch32_hal PRIVATE
        -march=rv32imac_zicsr
        -mabi=ilp32
        -mcmodel=medany
        -msmall-data-limit=8
        -mno-save-restore
)

# compiler_compat.h 经 __has_include 可选包含 config.h; 由 mini_tree 的
# mini_tree_gen custom target 产出, 此处建立编译期先后依赖。
add_dependencies(ch32_hal mini_tree_gen)
