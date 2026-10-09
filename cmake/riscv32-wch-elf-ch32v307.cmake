set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv)

set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

# ── 三端自动探测 RISC-V WCH 工具链 ──
# 兼容多种二进制命名约定：
#   riscv-wch-elf-        （WCH/MounRiver GCC12，官方推荐）
#   riscv32-wch-elf-      （WCH/MounRiver 旧命名）
#   riscv-none-elf-       （通用较新）
#   riscv-none-embed-     （Docker 镜像/旧版）
#   riscv64-unknown-elf-  （Debian/Ubuntu 系统包，需 picolibc）
#   riscv32-esp-elf-      （ESP-IDF 自带，含 picolibc/newlib）
# 探测顺序：/opt/wch-toolchain → Docker /opt → Linux 标准路径 → Windows 标准路径 → PATH
find_program(_CH32_RISCV_GCC
        NAMES riscv-wch-elf-gcc riscv32-wch-elf-gcc riscv32-esp-elf-gcc riscv-none-elf-gcc riscv-none-embed-gcc riscv64-unknown-elf-gcc
        HINTS
                "/opt/wch-toolchain/bin"
                "$ENV{CH32_RISCV_TOOLCHAIN_DIR}/bin"
                "$ENV{IDF_PATH}/tools/riscv32-esp-elf/bin"
                "$ENV{HOME}/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin"
                "/opt/toolchains/riscv/bin"
                "/opt/riscv/bin"
                "/opt/MounRiver/MounRiver_Studio/toolchain/riscv64-musl-1.0.3/bin"
                "/usr"
                "/usr/local"
                "D:/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC15/bin"
                "D:/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC12/bin"
                "D:/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin"
                "C:/MounRiver/MounRiver_Studio/toolchain/riscv64-musl-1.0.3/bin"
                "C:/Program Files/MounRiver/MounRiver_Studio/toolchain"
                "$ENV{MOUNRIVER_STUDIO_DIR}/toolchain"
        DOC "RISC-V WCH 交叉编译器 (gcc) 路径"
        REQUIRED)

get_filename_component(_CH32_RISCV_BINDIR "${_CH32_RISCV_GCC}" DIRECTORY)
get_filename_component(_CH32_RISCV_GCC_NAME "${_CH32_RISCV_GCC}" NAME)
# 由实际命中的 gcc 名反推工具链前缀 (gcc 前的部分, 兼容 Windows .exe 后缀)
string(REGEX REPLACE "gcc(\\.exe)?$" "" _CH32_RISCV_PREFIX "${_CH32_RISCV_GCC_NAME}")

# 使用 find_program 找每个工具, 自动处理 Windows .exe 后缀
find_program(_CH32_RISCV_GPP     NAMES "${_CH32_RISCV_PREFIX}g++"      HINTS "${_CH32_RISCV_BINDIR}" REQUIRED)
find_program(_CH32_RISCV_OBJCOPY NAMES "${_CH32_RISCV_PREFIX}objcopy"  HINTS "${_CH32_RISCV_BINDIR}" REQUIRED)
find_program(_CH32_RISCV_OBJDUMP NAMES "${_CH32_RISCV_PREFIX}objdump"  HINTS "${_CH32_RISCV_BINDIR}" REQUIRED)
find_program(_CH32_RISCV_SIZE    NAMES "${_CH32_RISCV_PREFIX}size"     HINTS "${_CH32_RISCV_BINDIR}" REQUIRED)

set(CMAKE_C_COMPILER   "${_CH32_RISCV_GCC}")
set(CMAKE_CXX_COMPILER "${_CH32_RISCV_GPP}")
set(CMAKE_ASM_COMPILER "${_CH32_RISCV_GCC}")
set(CMAKE_OBJCOPY      "${_CH32_RISCV_OBJCOPY}" CACHE INTERNAL "")
set(CMAKE_OBJDUMP      "${_CH32_RISCV_OBJDUMP}" CACHE INTERNAL "")
set(CMAKE_SIZE         "${_CH32_RISCV_SIZE}" CACHE INTERNAL "")
