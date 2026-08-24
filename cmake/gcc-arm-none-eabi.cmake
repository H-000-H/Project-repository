set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings
set(TOOLCHAIN_PREFIX                arm-none-eabi-)

# ── 三端自动探测编译器位置 ──
# Docker /opt → Linux 标准路径 → Windows 标准路径 → PATH
# 注意：含括号的 Windows 环境变量名需在 $ENV{} 中转义
find_program(_ARM_NONE_EABI_GCC
        NAMES "${TOOLCHAIN_PREFIX}gcc"
        HINTS
                "$ENV{ARM_NONE_EABI_TOOLCHAIN_DIR}/bin"
                "/opt/toolchains/gcc-arm-none-eabi/bin"
                "/opt/gcc-arm-none-eabi/bin"
                "/usr"
                "/usr/local"
                "C:/ST/STM32CubeCLT/GNU-tools-for-STM32/bin"
                "C:/ST/STM32CubeCLT_1.20.0/GNU-tools-for-STM32/bin"
                "$ENV{ProgramFiles}/ST/STM32CubeCLT/GNU-tools-for-STM32/bin"
                "C:/Program Files (x86)/GNU Tools Arm Embedded"
                "C:/Program Files/GNU Arm Embedded Toolchain"
                "$ENV{ProgramFiles}/GNU Arm Embedded Toolchain"
                "$ENV{ProgramFiles\(x86\)}/GNU Tools Arm Embedded"
        DOC "arm-none-eabi-gcc 交叉编译器路径"
        REQUIRED)

get_filename_component(_ARM_NONE_EABI_BINDIR "${_ARM_NONE_EABI_GCC}" DIRECTORY)

# 使用 find_program 找每个工具, 自动处理 Windows .exe 后缀
find_program(_ARM_NONE_EABI_GPP     NAMES "${TOOLCHAIN_PREFIX}g++"      HINTS "${_ARM_NONE_EABI_BINDIR}" REQUIRED)
find_program(_ARM_NONE_EABI_OBJCOPY NAMES "${TOOLCHAIN_PREFIX}objcopy"  HINTS "${_ARM_NONE_EABI_BINDIR}" REQUIRED)
find_program(_ARM_NONE_EABI_SIZE    NAMES "${TOOLCHAIN_PREFIX}size"     HINTS "${_ARM_NONE_EABI_BINDIR}" REQUIRED)
find_program(_ARM_NONE_EABI_OBJDUMP NAMES "${TOOLCHAIN_PREFIX}objdump"  HINTS "${_ARM_NONE_EABI_BINDIR}" REQUIRED)

set(CMAKE_C_COMPILER                "${_ARM_NONE_EABI_GCC}")
set(CMAKE_ASM_COMPILER              "${CMAKE_C_COMPILER}")
set(CMAKE_CXX_COMPILER              "${_ARM_NONE_EABI_GPP}")
set(CMAKE_LINKER                    "${_ARM_NONE_EABI_GPP}")
set(CMAKE_OBJCOPY                   "${_ARM_NONE_EABI_OBJCOPY}" CACHE INTERNAL "")
set(CMAKE_SIZE                      "${_ARM_NONE_EABI_SIZE}" CACHE INTERNAL "")
set(CMAKE_OBJDUMP                   "${_ARM_NONE_EABI_OBJDUMP}" CACHE INTERNAL "")

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU specific flags
set(TARGET_FLAGS "-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard ")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections")

set(CMAKE_C_FLAGS_DEBUG "-O2 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O3 -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32F407XX_FLASH.ld\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(TOOLCHAIN_LINK_LIBRARIES "m")
