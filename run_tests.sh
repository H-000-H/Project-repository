#!/usr/bin/env bash
# mini-os 宿主测试一键脚本 (Linux / macOS / MSYS / Git Bash)
#
# 用法 (仓库根目录下):
#   ./test/run_tests.sh                # 全部: rtos + mem(含 slab 变体) + idle
#   ./test/run_tests.sh rtos           # 只跑一个: rtos | mem | idle
#   CC=gcc ./test/run_tests.sh         # 指定编译器 (默认 clang, 无 clang 则 gcc)
#
# 产物与运行日志写入 build/; 任一用例编译或断言失败时脚本以非 0 退出。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build

# ------------------------------ 编译器定位 ---------------------------------
# Linux/macOS 用 PATH; Windows (Git Bash/MSYS) 兜底探测常见安装路径
if [ -n "${CC:-}" ]; then
    CC_BIN="$CC"
elif command -v clang >/dev/null 2>&1; then
    CC_BIN="clang"
elif [ -x "/c/Program Files/LLVM/bin/clang.exe" ]; then
    CC_BIN="/c/Program Files/LLVM/bin/clang.exe"
elif command -v gcc >/dev/null 2>&1; then
    CC_BIN="gcc"
elif [ -x "/c/Qt/Tools/mingw1310_64/bin/gcc.exe" ]; then
    CC_BIN="/c/Qt/Tools/mingw1310_64/bin/gcc.exe"
    PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH"   # cc1.exe 依赖其 bin 下的 DLL
else
    echo "找不到编译器 (clang / gcc 都不可用)" >&2
    exit 1
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "找不到编译器: $CC_BIN" >&2
    exit 1
fi
echo "compiler: $CC_BIN"

# ------------------------------ 测试定义 -----------------------------------
# 公共: -include redef.h 先注入 test/redef.h 宿主桩 (屏蔽 inc/redef.h 的
# Cortex-M 内联汇编); 内核特性宏默认全关, 全功能测试必须显式打开
COMMON=(-std=c11 -Wall -Wextra -Wno-unused-parameter -include test/redef.h -Iinc -Itest)
FEAT_FULL=(-DMINI_OS_EVENT=1 -DMINI_OS_FIND_BY_NAME=1 -DMINI_OS_TIME_SLICE=1 -DMINI_OS_THREAD_DETACH=1)

run_one() {
    local name="$1"; shift
    local exe="build/test_${name}.exe"
    local log="build/run_${name}.log"

    echo "==== [${name}] compiling ===="
    if ! "$CC_BIN" "${COMMON[@]}" "$@" -o "$exe" 2> "build/cc_${name}.err"; then
        echo "==== [${name}] BUILD FAILED ===="
        cat "build/cc_${name}.err"
        return 1
    fi

    if ! "$exe" > "$log" 2> "build/run_${name}.err"; then
        echo "==== [${name}] RUN crashed (exit $?) ===="
        return 1
    fi

    local fails
    fails=$(grep -c '^FAIL' "$log" || true)
    echo "==== [${name}] $(tail -n 1 "$log") ===="
    if [ "$fails" -gt 0 ]; then
        grep '^FAIL' "$log" | head -n 20
        return 1
    fi
    return 0
}

run_rtos() {
    run_one rtos \
        -DMINI_OS_EVENT=1 -DMINI_OS_FIND_BY_NAME=1 \
        -DMINI_OS_TIME_SLICE=1 -DMINI_OS_THREAD_DETACH=1 \
        test/test_rtos.c src/schedule.c src/thread.c src/mutex.c \
        src/semaphore.c src/queue.c src/event.c src/timer.c src/memory.c
}

run_mem() {
    run_one mem test/test_mem.c src/memory.c
}

# 注意页大小宏是 CONFIG_MINI_OS_SLAB_PAGE_SIZE (mem_heap.h 读取), 256 B 页
# 才能让 1 KB 池划出页; 写错旧名会静默用 2 KB 默认页导致 init 失败
run_mem_slab() {
    run_one mem_slab -DCONFIG_OPEN_SLAB -DCONFIG_MINI_OS_SLAB_PAGE_SIZE=256 \
        test/test_mem.c src/memory.c
}

run_idle() {
    run_one idle -DMINI_OS_THREAD_DETACH=1 \
        test/test_idle.c src/thread.c src/schedule.c src/mutex.c \
        src/semaphore.c src/timer.c src/memory.c
}

# ------------------------------ 调度 ----------------------------------------
WHAT="${1:-all}"
failed=0

case "$WHAT" in
    rtos) run_rtos || failed=$((failed + 1)) ;;
    mem)  run_mem || failed=$((failed + 1)); run_mem_slab || failed=$((failed + 1)) ;;
    idle) run_idle || failed=$((failed + 1)) ;;
    all)
        run_rtos     || failed=$((failed + 1))
        run_mem      || failed=$((failed + 1))
        run_mem_slab || failed=$((failed + 1))
        run_idle     || failed=$((failed + 1))
        ;;
    *)
        echo "用法: $0 [rtos|mem|idle]   (缺省跑全部)" >&2
        exit 1
        ;;
esac

echo
if [ "$failed" -gt 0 ]; then
    echo "RESULT: $failed test(s) FAILED"
    exit 1
fi
echo "RESULT: all tests passed"
exit 0
