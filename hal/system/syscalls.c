/* SPDX-License-Identifier: Apache-2.0 */
/*
 * syscalls.c — newlib 最小系统调用桩（替代 Cube 生成的 syscalls.c / sysmem.c）
 * Core/ 已移除，由平台自持：
 *   - 堆区 [_end, _estack)，超出返回 ENOMEM
 *   - printf/_write 接 USB CDC 虚拟串口 (DTS label "console")；
 *     USB 未就绪/未枚举时静默丢弃，不阻塞调用方。
 *     osal_log → vprintf → _write 链路因此自动落到串口。
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "device.h"
#include "status.h"

extern char _end asm("_end");       /* 堆起点（链接脚本 .bss 末尾） */
extern char _estack asm("_estack"); /* 栈顶（链接脚本 RAM 顶） */

/**
 * @brief 堆扩展 — malloc/free 依赖
 */
void* _sbrk(ptrdiff_t incr)
{
    static char* heap_end = 0;
    char* prev;

    if (heap_end == 0)
        heap_end = &_end;
    prev = heap_end;
    if (heap_end + incr > &_estack)
    {
        errno = ENOMEM;
        return (void*)-1;
    }
    heap_end += incr;
    return prev;
}

int _close(int file)
{
    (void)file;
    errno = ENOSYS;
    return -1;
}

int _fstat(int file, struct stat* st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _read(int file, char* ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

/* 控制台设备缓存: 首次使用时按 DTS label "console" 懒查找 + 打开 */
static struct device* s_console_dev;

/**
 * @brief 获取控制台设备 (USB CDC); 未就绪返回 NULL (丢弃输出)
 */
static struct device* console_dev_get(void)
{
    if (s_console_dev != NULL)
    {
        return s_console_dev;
    }

    struct device* pdev = device_find_by_label("console");
    if (IS_ERR(pdev))
    {
        return NULL; /* USB 未配置/未 probe — 静默丢弃 */
    }
    if (device_open(pdev, NULL) != MINI_OK)
    {
        return NULL;
    }
    s_console_dev = pdev;
    return s_console_dev;
}

int _write(int file, const char* ptr, int len)
{
    (void)file;
    if (len <= 0)
    {
        return len;
    }

    struct device* dev = console_dev_get();
    if (dev == NULL)
    {
        return len; /* 控制台未就绪: 吞掉输出避免卡死 */
    }

    /* 终端换行: '\n' → "\r\n" (分段写, 免大缓冲) */
    const char* seg = ptr;
    for (int i = 0; i < len; i++)
    {
        if (ptr[i] != '\n')
        {
            continue;
        }
        if (i > (int)(seg - ptr))
        {
            (void)device_write(dev, seg, (size_t)(ptr + i - seg), 0);
        }
        (void)device_write(dev, "\r\n", 2, 0);
        seg = ptr + i + 1;
    }
    if (seg < ptr + len)
    {
        (void)device_write(dev, seg, (size_t)(ptr + len - seg), 0);
    }
    return len;
}

int _getpid(void)
{
    return 1;
}

void _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
}
