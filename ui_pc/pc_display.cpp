/* Win32 窗口后端: 离屏 DIB 收 UI 刷屏, WM_PAINT 贴到窗口; 顺带能存 BMP 截图 */
#include "pc_display.h"

#define UNICODE
#define _UNICODE
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "pc_input.h"

namespace
{
    const wchar_t* const kClassName = L"PcUiPanelWindow";

    HWND      g_hwnd    = nullptr;
    HDC       g_mem_dc  = nullptr;
    HBITMAP   g_bitmap  = nullptr;
    uint32_t* g_pixels  = nullptr; /**< 32bpp, 每像素 0x00RRGGBB */
    bool      g_quit    = false;

    bool          g_mouse_down  = false;
    void (*g_frame_hook)(void)  = nullptr;
    bool          g_hook_called = false;

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(hwnd, &ps);
            BitBlt(dc, 0, 0, kPcPanelWidth, kPcPanelHeight, g_mem_dc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
            /* 客户区左上角 = 屏 (0,0), 坐标正好 1:1 给 LVGL */
            pc_input_mouse_update(static_cast<int>(static_cast<short>(LOWORD(lp))),
                                  static_cast<int>(static_cast<short>(HIWORD(lp))), g_mouse_down);
            return 0;
        case WM_LBUTTONDOWN:
            g_mouse_down = true;
            SetCapture(hwnd); /* 拖到窗口外也能收到松开 */
            pc_input_mouse_update(static_cast<int>(static_cast<short>(LOWORD(lp))),
                                  static_cast<int>(static_cast<short>(HIWORD(lp))), true);
            std::printf("[pc_display] mouse down at %d,%d\n", static_cast<int>(static_cast<short>(LOWORD(lp))),
                        static_cast<int>(static_cast<short>(HIWORD(lp))));
            return 0;
        case WM_LBUTTONUP:
            g_mouse_down = false;
            ReleaseCapture();
            pc_input_mouse_update(static_cast<int>(static_cast<short>(LOWORD(lp))),
                                  static_cast<int>(static_cast<short>(HIWORD(lp))), false);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                g_quit = true;
            }
            return 0;
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_DESTROY:
            g_quit = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void store_bmp(const char* path)
    {
        std::FILE* fp = std::fopen(path, "wb");
        if (fp == nullptr)
        {
            std::printf("[pc_display] 存图失败: %s\n", path);
            return;
        }

        const std::uint32_t img_bytes = static_cast<std::uint32_t>(kPcPanelWidth) * kPcPanelHeight * 4U;
        BITMAPFILEHEADER    file_hdr{};
        BITMAPINFOHEADER    info_hdr{};
        info_hdr.biSize        = sizeof(BITMAPINFOHEADER);
        info_hdr.biWidth       = kPcPanelWidth;
        info_hdr.biHeight      = kPcPanelHeight; /* 正数 = 自底向上, 下面按行倒着写 */
        info_hdr.biPlanes      = 1U;
        info_hdr.biBitCount    = 32U;
        info_hdr.biCompression = BI_RGB;
        file_hdr.bfType        = 0x4D42U; /* "BM" */
        file_hdr.bfOffBits     = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file_hdr.bfSize        = file_hdr.bfOffBits + img_bytes;

        std::fwrite(&file_hdr, sizeof(file_hdr), 1U, fp);
        std::fwrite(&info_hdr, sizeof(info_hdr), 1U, fp);
        for (int y = kPcPanelHeight - 1; y >= 0; --y)
        {
            std::fwrite(g_pixels + (static_cast<std::size_t>(y) * kPcPanelWidth), sizeof(std::uint32_t),
                        static_cast<std::size_t>(kPcPanelWidth), fp);
        }
        std::fclose(fp);
        std::printf("[pc_display] 已存截图: %s\n", path);
    }
} // namespace

void pc_display_set_first_frame_hook(void (*hook)(void))
{
    g_frame_hook = hook;
}

void pc_display_open(bool hidden)
{
    if (g_hwnd != nullptr)
    {
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    /* 让客户区正好是面板尺寸 */
    RECT rect{0, 0, kPcPanelWidth, kPcPanelHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, kClassName, L"ui_logic (PC)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (g_hwnd == nullptr)
    {
        std::printf("[pc_display] CreateWindow 失败: %lu\n", GetLastError());
        return;
    }

    /* 离屏位图: 顶向下 32bpp, 直接往 g_pixels 写 */
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = kPcPanelWidth;
    bi.bmiHeader.biHeight      = -kPcPanelHeight;
    bi.bmiHeader.biPlanes      = 1U;
    bi.bmiHeader.biBitCount    = 32U;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    g_mem_dc   = CreateCompatibleDC(screen);
    g_bitmap   = CreateDIBSection(g_mem_dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&g_pixels), nullptr, 0U);
    ReleaseDC(nullptr, screen);

    if ((g_mem_dc == nullptr) || (g_bitmap == nullptr) || (g_pixels == nullptr))
    {
        std::printf("[pc_display] DIB 创建失败\n");
        return;
    }
    SelectObject(g_mem_dc, g_bitmap);

    if (!hidden)
    {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }
    std::printf("[pc_display] 窗口就绪 %dx%d hwnd=0x%p%s\n", kPcPanelWidth, kPcPanelHeight,
                static_cast<void*>(g_hwnd), hidden ? " (隐藏)" : "");
}

void pc_display_blit(int x, int y, int w, int h, const uint16_t* px, bool byte_swapped)
{
    if ((g_pixels == nullptr) || (px == nullptr) || (w <= 0) || (h <= 0))
    {
        return;
    }

    for (int row = 0; row < h; ++row)
    {
        const int dy = y + row;
        if ((dy < 0) || (dy >= kPcPanelHeight))
        {
            continue;
        }
        for (int col = 0; col < w; ++col)
        {
            const int dx = x + col;
            if ((dx < 0) || (dx >= kPcPanelWidth))
            {
                continue;
            }

            const std::uint8_t b0 = reinterpret_cast<const std::uint8_t*>(px)[(static_cast<std::size_t>(row) * w + col) * 2U];
            const std::uint8_t b1 = reinterpret_cast<const std::uint8_t*>(px)[(static_cast<std::size_t>(row) * w + col) * 2U + 1U];
            /* RGB565_SWAPPED: 面板要高字节在前, 这里按大端还原成 RGB565 数值 */
            const std::uint16_t v = byte_swapped ? static_cast<std::uint16_t>((b0 << 8) | b1)
                                                 : static_cast<std::uint16_t>((b1 << 8) | b0);
            const std::uint32_t r5 = (v >> 11) & 0x1FU;
            const std::uint32_t g6 = (v >> 5) & 0x3FU;
            const std::uint32_t b5 = v & 0x1FU;
            const std::uint32_t r8 = (r5 << 3) | (r5 >> 2);
            const std::uint32_t g8 = (g6 << 2) | (g6 >> 4);
            const std::uint32_t b8 = (b5 << 3) | (b5 >> 2);

            g_pixels[static_cast<std::size_t>(dy) * kPcPanelWidth + dx] = (r8 << 16) | (g8 << 8) | b8;
        }
    }

    if (!g_hook_called && (g_frame_hook != nullptr))
    {
        g_hook_called = true;
        g_frame_hook(); /* 跑在 UI 线程: 在这儿建 LVGL 输入设备 */
    }

    if (!g_quit)
    {
        RECT dirty{x, y, x + w, y + h};
        InvalidateRect(g_hwnd, &dirty, FALSE);
    }
}

void pc_display_pump(uint32_t run_ms, const char* bmp_path)
{
    const DWORD start = GetTickCount();

    while (!g_quit)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit)
        {
            break;
        }
        if ((run_ms > 0U) && ((GetTickCount() - start) >= run_ms))
        {
            break;
        }
        Sleep(5U);
    }

    if ((bmp_path != nullptr) && (g_pixels != nullptr))
    {
        store_bmp(bmp_path);
    }
}

void pc_display_close(void)
{
    if (g_bitmap != nullptr)
    {
        DeleteObject(g_bitmap);
        g_bitmap = nullptr;
        g_pixels = nullptr;
    }
    if (g_mem_dc != nullptr)
    {
        DeleteDC(g_mem_dc);
        g_mem_dc = nullptr;
    }
    if (g_hwnd != nullptr)
    {
        if (GetCapture() == g_hwnd)
        {
            ReleaseCapture();
        }
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}
