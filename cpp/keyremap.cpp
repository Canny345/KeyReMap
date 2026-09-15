// ============================================================================
//  KeyRemap - 原生 Win32 按键映射工具
//  把 QWERTY 行的 12 个键（Q~P + [ ]）在「正常 / 数字 / 功能」三种模式间切换映射
//
//  数字模式： Q W E R T Y U I O P [ ]  ->  1 2 3 4 5 6 7 8 9 0 - =
//  功能模式： Q W E R T Y U I O P [ ]  ->  F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 F11 F12
//
//  编译：见 build_cpp.cmd（cl /O2 /MT /utf-8，静态链接 CRT，零依赖）
// ============================================================================

// 注意：不能定义 WIN32_LEAN_AND_MEAN —— 它会把 objidl.h 排除掉，
// 而 GDI+ 的头文件需要 IStream / IUnknown，会导致上百个编译错误
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

// ============================== 配置区 ==============================
static const UINT  kHotkeyMods = MOD_CONTROL | MOD_ALT;
static const UINT  kHotkeyVK   = 'M';
static const WCHAR* kHotkeyName = L"Ctrl+Alt+M";

// 提示框外观：全部按屏幕比例计算，自动适配任意分辨率 / 缩放 / 显示器
static const double kWidthRatio = 0.29;   // 宽度 = 工作区宽度的 29%
static const int    kWidthMin   = 300;    // 宽度下限（逻辑像素）
static const int    kWidthMax   = 560;    // 宽度上限（逻辑像素）
static const double kHeightOf   = 0.19;   // 高度 = 宽度 × 0.19
static const double kTopOf      = 0.06;   // 距工作区顶部 = 工作区高度的 6%
static const double kFontOf     = 0.30;   // 字号 = 高度 × 0.30（与已验证的字号观感一致）
static const BYTE   kBgAlpha    = 205;    // 背景不透明度 0-255；文字始终完全不透明
static const UINT   kModeHoldMs = 1300;   // 模式提示自动消失时间
// "按住型"提示的兜底超时：正常靠松开的 keyup 事件关闭，
// 此值仅在极端情况下（如 Ctrl+Alt+Del 安全桌面夺走键盘、keyup 丢失）兜底
static const UINT   kHoldSafetyMs = 15000;

static const COLORREF kBgColor[3] = {
    RGB(0x1E, 0x1E, 0x1E),   // 正常
    RGB(0x0B, 0x3D, 0x91),   // 数字
    RGB(0x14, 0x53, 0x2D),   // 功能
};
static const WCHAR* kModeName[3] = { L"正常", L"数字", L"功能" };

enum { MODE_NORMAL = 0, MODE_NUM = 1, MODE_FUNC = 2 };

// 映射表：字母排 12 键 对齐 数字排 12 键
static const int kRowVK[12] = {
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    VK_OEM_4,   // [
    VK_OEM_6,   // ]
};
static const int kNumTarget[12] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    VK_OEM_MINUS,   // -
    VK_OEM_PLUS,    // =
};
static const int kFnTarget[12] = {
    VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
    VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
};
static const WCHAR* kRowName[12] = {
    L"Q", L"W", L"E", L"R", L"T", L"Y", L"U", L"I", L"O", L"P", L"[", L"]",
};
// 数字键按住 Shift 时的显示符号（实际输入由物理 Shift 自然产生）
static const WCHAR* kShiftSym[12] = {
    L"!", L"@", L"#", L"$", L"%", L"^", L"&", L"*", L"(", L")", L"_", L"+",
};

enum {
    TIMER_TOAST = 1,
    WM_TRAYMSG  = WM_APP + 1,
    IDM_TOGGLE  = 1000,
    IDM_EXIT    = 1001,
};

// ============================== 全局状态 ==============================
static HINSTANCE g_hInst    = nullptr;
static HWND      g_hwnd     = nullptr;   // 隐藏的消息窗口
static HWND      g_hToast   = nullptr;   // 提示框（分层窗口）
static HHOOK     g_hook     = nullptr;
static int       g_mode     = MODE_NORMAL;

static std::unordered_map<int, int> g_curMap;   // 物理VK -> 目标VK
static std::unordered_map<int, int> g_held;     // 正在按住的 物理VK -> 目标VK

static int   g_toastX = 0, g_toastY = 0, g_toastW = 0, g_toastH = 0, g_fontH = 22;
static int   g_holdKey = 0;            // 当前"按住型"提示对应的物理VK（0 = 非按住型）
static bool  g_toastVisible = false;
static std::wstring g_lastToastText;

static NOTIFYICONDATAW g_nid = {};

// ============================== 工具函数 ==============================
static std::wstring Fmt(const WCHAR* fmt, ...) {
    WCHAR buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::wstring(buf);
}

// 发送目标按键。dwExtraInfo 打上标记，钩子里据此忽略"自己发出"的事件，
// 从而既能避免死循环，又不会误伤其他程序（宏工具、输入法等）注入的按键
static const ULONG_PTR kInjectedTag = 0x1A2B3C4D;

static void SendKey(int vk, bool isDown) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = kInjectedTag;
    SendInput(1, &in, sizeof(INPUT));
}

static bool IsShiftDown() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

// 计算提示框几何尺寸：以工作区为基准按比例算，换任何机器都自适应
static void ComputeGeometry() {
    POINT pt = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(mon, &mi);
    const RECT wa = mi.rcWork;                       // 物理像素（进程已 DPI 感知）
    const int workW = wa.right - wa.left;
    const int workH = wa.bottom - wa.top;

    const UINT dpi = GetDpiForSystem();
    const double scale = dpi / 96.0;

    int w = (int)(workW * kWidthRatio + 0.5);
    const int wMin = (int)(kWidthMin * scale + 0.5);
    const int wMax = (int)(kWidthMax * scale + 0.5);
    if (w < wMin) w = wMin;
    if (w > wMax) w = wMax;

    g_toastW = w;
    g_toastH = (int)(w * kHeightOf + 0.5);
    g_toastX = wa.left + (workW - g_toastW) / 2;     // 水平居中
    g_toastY = wa.top + (int)(workH * kTopOf + 0.5); // 顶部按比例留白
    g_fontH  = (int)(g_toastH * kFontOf + 0.5);
}

// ============================== 提示框绘制 ==============================
// 用 UpdateLayeredWindow + GDI+ 逐像素 Alpha：
// 背景半透明（kBgAlpha），文字完全不透明 —— 这样既透光又不糊字
static void AddRoundRect(Gdiplus::GraphicsPath& p, int x, int y, int w, int h, int r) {
    int d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void RenderAndShow(const std::wstring& text) {
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_toastW;
    bi.bmiHeader.biHeight      = -g_toastH;   // 自上而下
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbm);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        // 背景：半透明 + 圆角
        const COLORREF c = kBgColor[g_mode];
        Gdiplus::SolidBrush bg(Gdiplus::Color(kBgAlpha, GetRValue(c), GetGValue(c), GetBValue(c)));
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, 0, 0, g_toastW, g_toastH, (int)(g_toastH * 0.30));
        g.FillPath(&bg, &path);

        // 文字：完全不透明，保证清晰
        Gdiplus::FontFamily yahei(L"Microsoft YaHei");
        const Gdiplus::FontFamily* fam = (yahei.GetLastStatus() == Gdiplus::Ok)
            ? static_cast<const Gdiplus::FontFamily*>(&yahei)
            : Gdiplus::FontFamily::GenericSansSerif();
        Gdiplus::Font font(fam, (Gdiplus::REAL)g_fontH, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush fg(Gdiplus::Color(255, 255, 255, 255));

        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rc(0.0f, 0.0f, (Gdiplus::REAL)g_toastW, (Gdiplus::REAL)g_toastH);
        g.DrawString(text.c_str(), -1, &font, rc, &sf, &fg);
    }

    POINT src = { 0, 0 };
    POINT dst = { g_toastX, g_toastY };
    SIZE  sz  = { g_toastW, g_toastH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hToast, hdcScreen, &dst, &sz, hdcMem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

// holdKeyVK != 0 表示"按住保持型"提示（松开即消失）；autoHideMs > 0 表示定时自动消失
//
// 注意：这里不能用 GetAsyncKeyState 轮询判断物理键是否还按着 ——
// 钩子对原始按键返回 1 拦截后，该按键不会进入系统输入队列，
// 异步键状态永远查不到它。松开的判定必须以钩子收到的 keyup 事件为准。
static void ShowToast(const std::wstring& text, int holdKeyVK, UINT autoHideMs) {
    // 键盘自动重复会高频触发，内容没变就不重绘，避免闪动
    if (g_toastVisible && g_lastToastText == text && g_holdKey == holdKeyVK)
        return;

    g_lastToastText = text;
    g_holdKey = holdKeyVK;
    RenderAndShow(text);
    ShowWindow(g_hToast, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hToast, HWND_TOPMOST, g_toastX, g_toastY, g_toastW, g_toastH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_toastVisible = true;

    KillTimer(g_hwnd, TIMER_TOAST);
    if (autoHideMs) {
        SetTimer(g_hwnd, TIMER_TOAST, autoHideMs, nullptr);
    } else if (holdKeyVK != 0) {
        // 按住型：设一个很长的兜底超时，正常情况由 keyup 提前关闭
        SetTimer(g_hwnd, TIMER_TOAST, kHoldSafetyMs, nullptr);
    }
}

static void HideToast() {
    KillTimer(g_hwnd, TIMER_TOAST);
    g_holdKey = 0;
    g_toastVisible = false;
    g_lastToastText.clear();
    ShowWindow(g_hToast, SW_HIDE);
}

// ============================== 托盘 ==============================
static void SetTrayIcon() {
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    HICON ic = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_NORMAL + g_mode),
                                 IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!ic) ic = LoadIconW(nullptr, IDI_APPLICATION);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = ic;
    wcsncpy_s(g_nid.szTip, Fmt(L"按键映射 · %s模式（%s 切换）",
                               kModeName[g_mode], kHotkeyName).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTray() {
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    SetTrayIcon();
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    const std::wstring head = Fmt(L"当前模式：%s", kModeName[g_mode]);
    const std::wstring item = Fmt(L"切换模式   (%s)", kHotkeyName);
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, head.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, item.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

// ============================== 模式切换 ==============================
static void RebuildMap() {
    g_curMap.clear();
    if (g_mode == MODE_NUM) {
        for (int i = 0; i < 12; ++i) g_curMap[kRowVK[i]] = kNumTarget[i];
    } else if (g_mode == MODE_FUNC) {
        for (int i = 0; i < 12; ++i) g_curMap[kRowVK[i]] = kFnTarget[i];
    }
}

static void SetMode(int m) {
    // 释放仍按住的键，避免切模式后卡键
    for (auto& kv : g_held) SendKey(kv.second, false);
    g_held.clear();

    g_mode = m;
    RebuildMap();
    SetTrayIcon();
}

static void CycleMode() {
    const int m = (g_mode + 1) % 3;
    SetMode(m);
    ShowToast(Fmt(L"模式：%s", kModeName[m]), 0, kModeHoldMs);
}

// ============================== 键盘钩子 ==============================
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
    // 只忽略"我们自己注入"的事件，否则会死循环。
    // 注意不能用 LLKHF_INJECTED 一刀切，那会连带忽略其他程序注入的按键
    if (kb->dwExtraInfo == kInjectedTag)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    if (g_mode == MODE_NORMAL)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const int vk = (int)kb->vkCode;
    auto it = g_curMap.find(vk);
    if (it == g_curMap.end())
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    const int  target = it->second;

    if (isDown) {
        SendKey(target, true);
        g_held[vk] = target;
        // 找到提示文字用的下标
        for (int i = 0; i < 12; ++i) {
            if (kRowVK[i] == vk) {
                const WCHAR* shown = kShiftSym[i];
                // 功能模式不受 Shift 影响，显示 F 键名
                std::wstring dst = (g_mode == MODE_NUM)
                    ? std::wstring(IsShiftDown() ? shown : L"")
                    : std::wstring();
                if (dst.empty()) {
                    if (g_mode == MODE_NUM) {
                        // 数字/符号的原始字符
                        const int t = kNumTarget[i];
                        WCHAR b[8] = {};
                        if (t >= '0' && t <= '9') { b[0] = (WCHAR)t; }
                        else if (t == VK_OEM_MINUS) { b[0] = L'-'; }
                        else { b[0] = L'='; }
                        dst = b;
                    } else {
                        dst = Fmt(L"F%d", i + 1);
                    }
                }
                ShowToast(Fmt(L"%s   →   %s", kRowName[i], dst.c_str()), vk, 0);
                break;
            }
        }
        return 1;   // 拦截原始按键
    }

    if (isUp) {
        auto h = g_held.find(vk);
        if (h != g_held.end()) {
            SendKey(h->second, false);
            g_held.erase(h);
        }
        if (g_held.empty()) HideToast();
        return 1;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ============================== 窗口过程 ==============================
static LRESULT CALLBACK MsgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == 1) CycleMode();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TOGGLE) CycleMode();
        else if (LOWORD(wp) == IDM_EXIT) DestroyWindow(h);
        return 0;

    case WM_TRAYMSG:
        if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) CycleMode();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_TOAST) HideToast();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK ToastWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;   // 绝不抢焦点
    if (msg == WM_NCHITTEST)     return HTTRANSPARENT;   // 鼠标穿透
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================== 入口 ==============================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    // 单实例
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"KeyRemap_SingleInstance_Mutex");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

    // 隐藏的消息窗口
    WNDCLASSEXW mwc = { sizeof(WNDCLASSEXW) };
    mwc.lpfnWndProc   = MsgWndProc;
    mwc.hInstance     = hInst;
    mwc.lpszClassName = L"KeyRemapMsgClass";
    RegisterClassExW(&mwc);
    g_hwnd = CreateWindowExW(0, mwc.lpszClassName, L"KeyRemap", 0,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ComputeGeometry();

    // 提示框：分层 + 点击穿透 + 不抢焦点 + 不进 Alt+Tab
    WNDCLASSEXW twc = { sizeof(WNDCLASSEXW) };
    twc.lpfnWndProc   = ToastWndProc;
    twc.hInstance     = hInst;
    twc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    twc.lpszClassName = L"KeyRemapToastClass";
    RegisterClassExW(&twc);
    g_hToast = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        twc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (!g_hToast) return 1;

    AddTray();

    if (!RegisterHotKey(g_hwnd, 1, kHotkeyMods, kHotkeyVK)) {
        MessageBoxW(nullptr,
            Fmt(L"热键 %s 已被其他程序占用，程序退出。\n请修改源码中的 kHotkeyVK / kHotkeyMods 后重新编译。",
                kHotkeyName).c_str(),
            L"KeyRemap", MB_ICONERROR);
        return 1;
    }

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"安装键盘钩子失败，程序退出。", L"KeyRemap", MB_ICONERROR);
        return 1;
    }

    SetMode(MODE_NORMAL);
    ShowToast(Fmt(L"正常模式 · %s 切换", kHotkeyName), 0, 2200);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    if (g_hook) UnhookWindowsHookEx(g_hook);
    for (auto& kv : g_held) SendKey(kv.second, false);
    g_held.clear();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    Gdiplus::GdiplusShutdown(gdipToken);
    if (mtx) CloseHandle(mtx);
    return 0;
}
