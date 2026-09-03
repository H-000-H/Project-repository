# mini-os 宿主测试一键脚本 (Windows PowerShell 5.1+)
#
# 用法 (仓库根目录下):
#   .\test\run_tests.ps1                  # 全部: rtos + mem(含 slab 变体) + idle
#   .\test\run_tests.ps1 -Test rtos       # 只跑一个: rtos | mem | idle
#   .\test\run_tests.ps1 -Compiler gcc    # 指定编译器: auto | clang | gcc
#
# 产物与运行日志写入 build\; 任一用例编译或断言失败时脚本以非 0 退出。
param(
    [ValidateSet('all', 'rtos', 'mem', 'idle')]
    [string]$Test = 'all',
    [ValidateSet('auto', 'clang', 'gcc')]
    [string]$Compiler = 'auto'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # 仓库根 (本脚本位于 test/)
Set-Location $root
New-Item -ItemType Directory -Force -Path build | Out-Null

# ------------------------------ 编译器定位 ---------------------------------
function Find-Compiler
{
    $candidates = @()
    if ($Compiler -ne 'gcc')
    {
        $candidates += @('clang', 'C:\Program Files\LLVM\bin\clang.exe')
    }
    if ($Compiler -ne 'clang')
    {
        $candidates += @('gcc', 'C:\Qt\Tools\mingw1310_64\bin\gcc.exe')
    }
    foreach ($c in $candidates)
    {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($null -ne $cmd)
        {
            return $cmd.Source
        }
    }
    throw "找不到编译器 (clang / gcc 都不可用), 请安装或用 -Compiler 指定"
}

$cc = Find-Compiler
# Qt 附带的 MinGW gcc: cc1.exe 需要从其 bin 目录加载 DLL, PATH 缺该目录时
# gcc 本体正常但编译静默失败 (exit 1, 无任何输出), 必须前置 PATH
if ($cc -like '*mingw*')
{
    $bindir = Split-Path -Parent $cc
    if (($env:Path -split ';') -notcontains $bindir)
    {
        $env:Path = "$bindir;" + $env:Path
    }
}
Write-Output "compiler: $cc"

# ------------------------------ 测试定义 -----------------------------------
# 公共: -include redef.h 先注入 test/redef.h 宿主桩 (屏蔽 inc/redef.h 的
# Cortex-M 内联汇编); 内核特性宏默认全关, 全功能测试必须显式打开
$common = @('-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
            '-include', 'test/redef.h', '-Iinc', '-Itest')
$featFull = @('-DMINI_OS_EVENT=1', '-DMINI_OS_FIND_BY_NAME=1',
              '-DMINI_OS_TIME_SLICE=1', '-DMINI_OS_THREAD_DETACH=1')

$tests = @(
    @{
        name = 'rtos'
        src  = @('test/test_rtos.c', 'src/schedule.c', 'src/thread.c', 'src/mutex.c',
                 'src/semaphore.c', 'src/queue.c', 'src/event.c', 'src/timer.c',
                 'src/memory.c')
        defs = $featFull
    },
    @{
        name = 'mem'
        src  = @('test/test_mem.c', 'src/memory.c')
        defs = @()
    },
    @{
        name = 'mem_slab'
        src  = @('test/test_mem.c', 'src/memory.c')
        # 注意页大小宏是 CONFIG_MINI_OS_SLAB_PAGE_SIZE (mem_heap.h 读取),
        # 256 B 页才能让 1 KB 池划出页; 写错旧名会静默用 2 KB 默认页导致 init 失败
        defs = @('-DCONFIG_OPEN_SLAB', '-DCONFIG_MINI_OS_SLAB_PAGE_SIZE=256')
    },
    @{
        name = 'idle'
        src  = @('test/test_idle.c', 'src/thread.c', 'src/schedule.c',
                 'src/mutex.c', 'src/semaphore.c', 'src/timer.c', 'src/memory.c')
        defs = @('-DMINI_OS_THREAD_DETACH=1')
    }
)

if ($Test -ne 'all')
{
    $tests = $tests | Where-Object { $_.name -eq $Test -or ($Test -eq 'mem' -and $_.name -like 'mem*') }
}

# ------------------------------ 编译 + 运行 --------------------------------
$failed = 0
foreach ($t in $tests)
{
    $exe = "build/test_$($t.name).exe"
    $log = "build/run_$($t.name).log"
    Write-Output "==== [$($t.name)] compiling ===="
    & $cc @($common) @($t.defs) @($t.src) -o $exe 2> build\cc_$($t.name).err
    if ($LASTEXITCODE -ne 0)
    {
        Write-Output "==== [$($t.name)] BUILD FAILED ===="
        Get-Content build\cc_$($t.name).err
        $failed++
        continue
    }
    $p = Start-Process -FilePath $exe -RedirectStandardOutput $log `
         -RedirectStandardError build\run_$($t.name).err -NoNewWindow -PassThru -Wait
    if ($p.ExitCode -ne 0)
    {
        Write-Output "==== [$($t.name)] RUN crashed (exit $($p.ExitCode)) ===="
        $failed++
        continue
    }
    $fails = Select-String -Path $log -Pattern '^FAIL' | Measure-Object | Select-Object -ExpandProperty Count
    $summary = (Get-Content $log | Select-Object -Last 1)
    Write-Output "==== [$($t.name)] $summary ===="
    if ($fails -gt 0)
    {
        Select-String -Path $log -Pattern '^FAIL' | Select-Object -First 20 | ForEach-Object { Write-Output $_.Line }
        $failed++
    }
}

Write-Output ''
if ($failed -gt 0)
{
    Write-Output "RESULT: $failed test(s) FAILED"
    exit 1
}
Write-Output 'RESULT: all tests passed'
exit 0
