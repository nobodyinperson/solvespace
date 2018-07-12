//-----------------------------------------------------------------------------
// The Win32-based implementation of platform-dependent GUI functionality.
//
// Copyright 2018 whitequark
//-----------------------------------------------------------------------------
#include "config.h"
#include "solvespace.h"
// Include after solvespace.h to avoid identifier clashes.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED       0x02E0
#endif

// We have our own CreateWindow.
#undef CreateWindow

#if HAVE_OPENGL == 3
#define EGLAPI /*static linkage*/
#include <EGL/egl.h>
#endif

namespace SolveSpace {
namespace Platform {

//-----------------------------------------------------------------------------
// Windows API bridging
//-----------------------------------------------------------------------------

#define sscheck(expr) do {                                                    \
        SetLastError(0);                                                      \
        if(!(expr))                                                           \
            CheckLastError(__FILE__, __LINE__, __func__, #expr);              \
    } while(0)

void CheckLastError(const char *file, int line, const char *function, const char *expr) {
    if(GetLastError() != S_OK) {
        LPWSTR messageW;
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
                      NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      (LPWSTR)&messageW, 0, NULL);

        std::string message;
        message += ssprintf("File %s, line %u, function %s:\n", file, line, function);
        message += ssprintf("Win32 API call failed: %s.\n", expr);
        message += ssprintf("Error: %s", Narrow(messageW).c_str());
        FatalError(message);
    }
}

typedef UINT (WINAPI *LPFNGETDPIFORWINDOW)(HWND);

UINT ssGetDpiForWindow(HWND hwnd) {
    static bool checked;
    static LPFNGETDPIFORWINDOW lpfnGetDpiForWindow;
    if(!checked) {
        checked = true;
        lpfnGetDpiForWindow = (LPFNGETDPIFORWINDOW)
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    if(lpfnGetDpiForWindow) {
        return lpfnGetDpiForWindow(hwnd);
    } else {
        HDC hDc;
        sscheck(hDc = GetDC(HWND_DESKTOP));
        UINT dpi;
        sscheck(dpi = GetDeviceCaps(hDc, LOGPIXELSX));
        sscheck(ReleaseDC(HWND_DESKTOP, hDc));
        return dpi;
    }
}

typedef BOOL (WINAPI *LPFNADJUSTWINDOWRECTEXFORDPI)(LPRECT, DWORD, BOOL, DWORD, UINT);

BOOL ssAdjustWindowRectExForDpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu,
                                DWORD dwExStyle, UINT dpi) {
    static bool checked;
    static LPFNADJUSTWINDOWRECTEXFORDPI lpfnAdjustWindowRectExForDpi;
    if(!checked) {
        checked = true;
        lpfnAdjustWindowRectExForDpi = (LPFNADJUSTWINDOWRECTEXFORDPI)
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    }
    if(lpfnAdjustWindowRectExForDpi) {
        return lpfnAdjustWindowRectExForDpi(lpRect, dwStyle, bMenu, dwExStyle, dpi);
    } else {
        return AdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
    }
}

//-----------------------------------------------------------------------------
// Utility functions
//-----------------------------------------------------------------------------

std::wstring Title(const std::string &s) {
    return Widen("SolveSpace - " + s);
}

static int Clamp(int x, int a, int b) {
    return max(a, min(x, b));
}

//-----------------------------------------------------------------------------
// Timers
//-----------------------------------------------------------------------------

class TimerImplWin32 : public Timer {
public:
    static HWND WindowHandle() {
        static HWND hTimerWnd;
        if(hTimerWnd == NULL) {
            sscheck(hTimerWnd = CreateWindowExW(0, L"Message", NULL, 0, 0, 0, 0, 0,
                                                HWND_MESSAGE, NULL, NULL, NULL));
        }
        return hTimerWnd;
    }

    static void CALLBACK TimerFunc(HWND hwnd, UINT msg, UINT_PTR event, DWORD time) {
        sscheck(KillTimer(WindowHandle(), event));

        TimerImplWin32 *timer = (TimerImplWin32*)event;
        if(timer->onTimeout) {
            timer->onTimeout();
        }
    }

    void WindUp(unsigned milliseconds) override {
        // FIXME(platform/gui): use SetCoalescableTimer when it's available (8+)
        sscheck(SetTimer(WindowHandle(), (UINT_PTR)this,
                         milliseconds, &TimerImplWin32::TimerFunc));
    }

    ~TimerImplWin32() {
        // FIXME(platform/gui): there's a race condition here--WM_TIMER messages already
        // posted to the queue are not removed, so this destructor is at most "best effort".
        KillTimer(WindowHandle(), (UINT_PTR)this);
    }
};

TimerRef CreateTimer() {
    return std::unique_ptr<TimerImplWin32>(new TimerImplWin32);
}

//-----------------------------------------------------------------------------
// Menus
//-----------------------------------------------------------------------------

class MenuImplWin32;

class MenuItemImplWin32 : public MenuItem {
public:
    std::shared_ptr<MenuImplWin32> menu;

    HMENU Handle();

    MENUITEMINFOW GetInfo(UINT mask) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask  = mask;
        sscheck(GetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));
        return mii;
    }

    void SetAccelerator(KeyboardEvent accel) override {
        MENUITEMINFOW mii = GetInfo(MIIM_TYPE);

        std::wstring nameW(mii.cch, L'\0');
        mii.dwTypeData = &nameW[0];
        mii.cch++;
        sscheck(GetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));

        std::string name = Narrow(nameW);
        if(name.find('\t') != std::string::npos) {
            name = name.substr(0, name.find('\t'));
        }
        name += '\t';
        name += AcceleratorDescription(accel);

        nameW = Widen(name);
        mii.fMask      = MIIM_STRING;
        mii.dwTypeData = &nameW[0];
        sscheck(SetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));
    }

    void SetIndicator(Indicator type) override {
        MENUITEMINFOW mii = GetInfo(MIIM_FTYPE);
        switch(type) {
            case Indicator::NONE:
            case Indicator::CHECK_MARK:
                mii.fType &= ~MFT_RADIOCHECK;
                break;

            case Indicator::RADIO_MARK:
                mii.fType |= MFT_RADIOCHECK;
                break;
        }
        sscheck(SetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));
    }

    void SetActive(bool active) override {
        MENUITEMINFOW mii = GetInfo(MIIM_STATE);
        if(active) {
            mii.fState |= MFS_CHECKED;
        } else {
            mii.fState &= ~MFS_CHECKED;
        }
        sscheck(SetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));
    }

    void SetEnabled(bool enabled) override {
        MENUITEMINFOW mii = GetInfo(MIIM_STATE);
        if(enabled) {
            mii.fState &= ~(MFS_DISABLED|MFS_GRAYED);
        } else {
            mii.fState |= MFS_DISABLED|MFS_GRAYED;
        }
        sscheck(SetMenuItemInfoW(Handle(), (UINT_PTR)this, FALSE, &mii));
    }
};

int64_t contextMenuPopTime = 0;

class MenuImplWin32 : public Menu {
public:
    HMENU hMenu;

    std::weak_ptr<MenuImplWin32> weakThis;
    std::vector<std::shared_ptr<MenuItemImplWin32>> menuItems;
    std::vector<std::shared_ptr<MenuImplWin32>>     subMenus;

    MenuImplWin32() {
        sscheck(hMenu = CreatePopupMenu());
    }

    MenuItemRef AddItem(const std::string &label,
                        std::function<void()> onTrigger = NULL) override {
        auto menuItem = std::make_shared<MenuItemImplWin32>();
        menuItem->menu = weakThis.lock();
        menuItem->onTrigger = onTrigger;
        menuItems.push_back(menuItem);

        sscheck(AppendMenuW(hMenu, MF_STRING, (UINT_PTR)menuItem.get(), Widen(label).c_str()));

        // uID is just an UINT, which isn't large enough to hold a pointer on 64-bit Windows,
        // so we use dwItemData, which is.
        MENUITEMINFOW mii = {};
        mii.cbSize     = sizeof(mii);
        mii.fMask      = MIIM_DATA;
        mii.dwItemData = (LONG_PTR)menuItem.get();
        sscheck(SetMenuItemInfoW(hMenu, (UINT_PTR)menuItem.get(), FALSE, &mii));

        return menuItem;
    }

    MenuRef AddSubMenu(const std::string &label) override {
        auto subMenu = std::make_shared<MenuImplWin32>();
        subMenu->weakThis = subMenu;
        subMenus.push_back(subMenu);

        sscheck(AppendMenuW(hMenu, MF_STRING|MF_POPUP,
                            (UINT_PTR)subMenu->hMenu, Widen(label).c_str()));

        return subMenu;
    }

    void AddSeparator() override {
        sscheck(AppendMenuW(hMenu, MF_SEPARATOR, 0, L""));
    }

    void PopUp() override {
        MENUINFO mi = {};
        mi.cbSize  = sizeof(mi);
        mi.fMask   = MIM_APPLYTOSUBMENUS|MIM_STYLE;
        mi.dwStyle = MNS_NOTIFYBYPOS;
        sscheck(SetMenuInfo(hMenu, &mi));

        POINT pt;
        sscheck(GetCursorPos(&pt));

        sscheck(TrackPopupMenu(hMenu, TPM_TOPALIGN, pt.x, pt.y, 0, GetActiveWindow(), NULL));
        contextMenuPopTime = GetMilliseconds();
    }

    void Clear() override {
        for(int n = GetMenuItemCount(hMenu) - 1; n >= 0; n--) {
            sscheck(RemoveMenu(hMenu, n, MF_BYPOSITION));
        }
        menuItems.clear();
        subMenus.clear();
    }

    ~MenuImplWin32() {
        Clear();
        sscheck(DestroyMenu(hMenu));
    }
};

HMENU MenuItemImplWin32::Handle() {
    return menu->hMenu;
}

MenuRef CreateMenu() {
    auto menu = std::make_shared<MenuImplWin32>();
    // std::enable_shared_from_this fails for some reason, not sure why
    menu->weakThis = menu;
    return menu;
}

class MenuBarImplWin32 : public MenuBar {
public:
    HMENU hMenuBar;

    std::vector<std::shared_ptr<MenuImplWin32>> subMenus;

    MenuBarImplWin32() {
        sscheck(hMenuBar = ::CreateMenu());
    }

    MenuRef AddSubMenu(const std::string &label) override {
        auto subMenu = std::make_shared<MenuImplWin32>();
        subMenu->weakThis = subMenu;
        subMenus.push_back(subMenu);

        sscheck(AppendMenuW(hMenuBar, MF_STRING|MF_POPUP,
                            (UINT_PTR)subMenu->hMenu, Widen(label).c_str()));

        return subMenu;
    }

    void Clear() override {
        for(int n = GetMenuItemCount(hMenuBar) - 1; n >= 0; n--) {
            sscheck(RemoveMenu(hMenuBar, n, MF_BYPOSITION));
        }
        subMenus.clear();
    }

    ~MenuBarImplWin32() {
        Clear();
        sscheck(DestroyMenu(hMenuBar));
    }
};

MenuBarRef GetOrCreateMainMenu(bool *unique) {
    *unique = false;
    return std::make_shared<MenuBarImplWin32>();
}

//-----------------------------------------------------------------------------
// Windows
//-----------------------------------------------------------------------------

#define SCROLLBAR_UNIT 65536

class WindowImplWin32 : public Window {
public:
    HWND hWindow  = NULL;
    HWND hTooltip = NULL;
    HWND hEditor  = NULL;
    WNDPROC editorWndProc = NULL;

#if HAVE_OPENGL == 1
    HGLRC hGlRc = NULL;
#elif HAVE_OPENGL == 3
    EGLDisplay eglDisplay = NULL;
    EGLSurface eglSurface = NULL;
    EGLContext eglContext = NULL;
#endif

    WINDOWPLACEMENT placement = {};
    int minWidth = 0, minHeight = 0;

    std::shared_ptr<MenuBarImplWin32> menuBar;
    std::string tooltipText;
    bool scrollbarVisible = false;

    static void RegisterWindowClass() {
        static bool registered;
        if(registered) return;

        WNDCLASSEX wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_BYTEALIGNCLIENT|CS_BYTEALIGNWINDOW|CS_OWNDC|CS_DBLCLKS;
        wc.lpfnWndProc   = WndProc;
        wc.cbWndExtra    = sizeof(WindowImplWin32 *);
        wc.hIcon         = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(4000),
                                            IMAGE_ICON, 32, 32, 0);
        wc.hIconSm       = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(4000),
                                            IMAGE_ICON, 16, 16, 0);
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"SolveSpace";
        sscheck(RegisterClassEx(&wc));
        registered = true;
    }

    WindowImplWin32(Window::Kind kind, std::shared_ptr<WindowImplWin32> parentWindow) {
        placement.length = sizeof(placement);

        RegisterWindowClass();

        HWND hParentWindow = NULL;
        if(parentWindow) {
            hParentWindow = parentWindow->hWindow;
        }

        DWORD style = WS_SIZEBOX|WS_CLIPCHILDREN;
        switch(kind) {
            case Window::Kind::TOPLEVEL:
                style |= WS_OVERLAPPEDWINDOW|WS_CLIPSIBLINGS;
                break;

            case Window::Kind::TOOL:
                style |= WS_POPUPWINDOW|WS_CAPTION;
                break;
        }
        sscheck(hWindow = CreateWindowExW(0, L"SolveSpace", L"", style,
                                          CW_USEDEFAULT, CW_USEDEFAULT,
                                          CW_USEDEFAULT, CW_USEDEFAULT,
                                          hParentWindow, NULL, NULL, NULL));
        sscheck(SetWindowLongPtr(hWindow, 0, (LONG_PTR)this));
        if(hParentWindow != NULL) {
            sscheck(SetWindowPos(hWindow, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE));
        }

        sscheck(hTooltip = CreateWindowExW(0, TOOLTIPS_CLASS, NULL,
                                           WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,
                                           CW_USEDEFAULT, CW_USEDEFAULT,
                                           CW_USEDEFAULT, CW_USEDEFAULT,
                                           hWindow, NULL, NULL, NULL));
        sscheck(SetWindowPos(hTooltip, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE));

        TOOLINFOW ti = {};
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_IDISHWND|TTF_SUBCLASS;
        ti.hwnd     = hWindow;
        ti.uId      = (UINT_PTR)hWindow;
        ti.lpszText = (LPWSTR)L"";
        sscheck(SendMessageW(hTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti));
        sscheck(SendMessageW(hTooltip, TTM_ACTIVATE, FALSE, 0));

        DWORD editorStyle = WS_CLIPSIBLINGS|WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL;
        sscheck(hEditor = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDIT, L"", editorStyle,
                                          0, 0, 0, 0, hWindow, NULL, NULL, NULL));
        sscheck(editorWndProc =
                (WNDPROC)SetWindowLongPtr(hEditor, GWLP_WNDPROC, (LONG_PTR)EditorWndProc));

        HDC hDc;
        sscheck(hDc = GetDC(hWindow));

#if HAVE_OPENGL == 1
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize        = sizeof(PIXELFORMATDESCRIPTOR);
        pfd.nVersion     = 1;
        pfd.dwFlags      = PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
        pfd.dwLayerMask  = PFD_MAIN_PLANE;
        pfd.iPixelType   = PFD_TYPE_RGBA;
        pfd.cColorBits   = 32;
        pfd.cDepthBits   = 24;
        pfd.cAccumBits   = 0;
        pfd.cStencilBits = 0;
        int pixelFormat;
        sscheck(pixelFormat = ChoosePixelFormat(hDc, &pfd));
        sscheck(SetPixelFormat(hDc, pixelFormat, &pfd));

        sscheck(hGlRc = wglCreateContext(hDc));
#elif HAVE_OPENGL == 3
        ssassert(eglBindAPI(EGL_OPENGL_ES_API), "Cannot bind EGL API");

        eglDisplay = eglGetDisplay(hDc);
        ssassert(eglInitialize(eglDisplay, NULL, NULL), "Cannot initialize EGL");

        EGLint configAttributes[] = {
            EGL_COLOR_BUFFER_TYPE,  EGL_RGB_BUFFER,
            EGL_RED_SIZE,           8,
            EGL_GREEN_SIZE,         8,
            EGL_BLUE_SIZE,          8,
            EGL_DEPTH_SIZE,         24,
            EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
            EGL_NONE
        };
        EGLint numConfigs;
        EGLConfig windowConfig;
        ssassert(eglChooseConfig(eglDisplay, configAttributes, &windowConfig, 1, &numConfigs),
                 "Cannot choose EGL configuration");

        EGLint surfaceAttributes[] = {
            EGL_NONE
        };
        eglSurface = eglCreateWindowSurface(eglDisplay, windowConfig, hWindow, surfaceAttributes);
        ssassert(eglSurface != EGL_NO_SURFACE, "Cannot create EGL window surface");

        EGLint contextAttributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        eglContext = eglCreateContext(eglDisplay, windowConfig, NULL, contextAttributes);
        ssassert(eglContext != EGL_NO_CONTEXT, "Cannot create EGL context");
#endif

        sscheck(ReleaseDC(hWindow, hDc));
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
        if(handlingFatalError) return TRUE;

        WindowImplWin32 *window;
        sscheck(window = (WindowImplWin32 *)GetWindowLongPtr(h, 0));

        // The wndproc may be called from within CreateWindowEx, and before we've associated
        // the window with the WindowImplWin32. In that case, just defer to the default wndproc.
        if(window == NULL) {
            return DefWindowProc(h, msg, wParam, lParam);
        }

        switch (msg) {
            case WM_ERASEBKGND:
                break;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hDc = BeginPaint(window->hWindow, &ps);
                if(window->onRender) {
#if HAVE_OPENGL == 1
                    wglMakeCurrent(hDc, window->hGlRc);
#elif HAVE_OPENGL == 3
                    eglMakeCurrent(window->eglDisplay, window->eglSurface,
                                   window->eglSurface, window->eglContext);
#endif
                    window->onRender();
#if HAVE_OPENGL == 1
                    SwapBuffers(hDc);
#elif HAVE_OPENGL == 3
                    eglSwapBuffers(window->eglDisplay, window->eglSurface);
                    (void)hDc;
#endif
                }
                EndPaint(window->hWindow, &ps);
                break;
            }

            case WM_CLOSE:
            case WM_DESTROY:
                if(window->onClose) {
                    window->onClose();
                }
                break;

            case WM_SIZE:
                window->Invalidate();
                break;

            case WM_SIZING: {
                int pixelRatio = window->GetDevicePixelRatio();

                RECT rcw, rcc;
                sscheck(GetWindowRect(window->hWindow, &rcw));
                sscheck(GetClientRect(window->hWindow, &rcc));
                int nonClientWidth  = (rcw.right - rcw.left) - (rcc.right - rcc.left);
                int nonClientHeight = (rcw.bottom - rcw.top) - (rcc.bottom - rcc.top);

                RECT *rc = (RECT *)lParam;
                int adjWidth  = rc->right - rc->left;
                int adjHeight = rc->bottom - rc->top;

                adjWidth  -= nonClientWidth;
                adjWidth   = max(window->minWidth * pixelRatio, adjWidth);
                adjWidth  += nonClientWidth;
                adjHeight -= nonClientHeight;
                adjHeight  = max(window->minHeight * pixelRatio, adjHeight);
                adjHeight += nonClientHeight;
                switch(wParam) {
                    case WMSZ_RIGHT:
                    case WMSZ_BOTTOMRIGHT:
                    case WMSZ_TOPRIGHT:
                        rc->right = rc->left + adjWidth;
                        break;

                    case WMSZ_LEFT:
                    case WMSZ_BOTTOMLEFT:
                    case WMSZ_TOPLEFT:
                        rc->left = rc->right - adjWidth;
                        break;
                }
                switch(wParam) {
                    case WMSZ_BOTTOM:
                    case WMSZ_BOTTOMLEFT:
                    case WMSZ_BOTTOMRIGHT:
                        rc->bottom = rc->top + adjHeight;
                        break;

                    case WMSZ_TOP:
                    case WMSZ_TOPLEFT:
                    case WMSZ_TOPRIGHT:
                        rc->top = rc->bottom - adjHeight;
                        break;
                }
                break;
            }

            case WM_DPICHANGED: {
                RECT rc = *(RECT *)lParam;
                sscheck(SendMessage(window->hWindow, WM_SIZING, WMSZ_BOTTOMRIGHT, (LPARAM)&rc));
                sscheck(SetWindowPos(window->hWindow, NULL, rc.left, rc.top,
                                     rc.right - rc.left, rc.bottom - rc.top,
                                     SWP_NOZORDER|SWP_NOACTIVATE));
                window->Invalidate();
                break;
            }

            case WM_LBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_MBUTTONDBLCLK:
            case WM_RBUTTONDBLCLK:
            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
            case WM_RBUTTONUP:
                if(GetMilliseconds() - Platform::contextMenuPopTime < 100) {
                    // Ignore the mouse click that dismisses a context menu, to avoid
                    // (e.g.) clearing a selection.
                    return 0;
                }
                // fallthrough
            case WM_MOUSEMOVE:
            case WM_MOUSEWHEEL:
            case WM_MOUSELEAVE: {
                int pixelRatio = window->GetDevicePixelRatio();

                MouseEvent event = {};
                event.x = GET_X_LPARAM(lParam) / pixelRatio;
                event.y = GET_Y_LPARAM(lParam) / pixelRatio;
                event.button = MouseEvent::Button::NONE;

                event.shiftDown   = (wParam & MK_SHIFT) != 0;
                event.controlDown = (wParam & MK_CONTROL) != 0;

                switch(msg) {
                    case WM_LBUTTONDOWN:
                        event.button = MouseEvent::Button::LEFT;
                        event.type = MouseEvent::Type::PRESS;
                        break;
                    case WM_MBUTTONDOWN:
                        event.button = MouseEvent::Button::MIDDLE;
                        event.type = MouseEvent::Type::PRESS;
                        break;
                    case WM_RBUTTONDOWN:
                        event.button = MouseEvent::Button::RIGHT;
                        event.type = MouseEvent::Type::PRESS;
                        break;

                    case WM_LBUTTONDBLCLK:
                        event.button = MouseEvent::Button::LEFT;
                        event.type = MouseEvent::Type::DBL_PRESS;
                        break;
                    case WM_MBUTTONDBLCLK:
                        event.button = MouseEvent::Button::MIDDLE;
                        event.type = MouseEvent::Type::DBL_PRESS;
                        break;
                    case WM_RBUTTONDBLCLK:
                        event.button = MouseEvent::Button::RIGHT;
                        event.type = MouseEvent::Type::DBL_PRESS;
                        break;

                    case WM_LBUTTONUP:
                        event.button = MouseEvent::Button::LEFT;
                        event.type = MouseEvent::Type::RELEASE;
                        break;
                    case WM_MBUTTONUP:
                        event.button = MouseEvent::Button::MIDDLE;
                        event.type = MouseEvent::Type::RELEASE;
                        break;
                    case WM_RBUTTONUP:
                        event.button = MouseEvent::Button::RIGHT;
                        event.type = MouseEvent::Type::RELEASE;
                        break;

                    case WM_MOUSEWHEEL:
                        // Make the mousewheel work according to which window the mouse is
                        // over, not according to which window is active.
                        POINT pt;
                        pt.x = LOWORD(lParam);
                        pt.y = HIWORD(lParam);
                        HWND hWindowUnderMouse;
                        sscheck(hWindowUnderMouse = WindowFromPoint(pt));
                        if(hWindowUnderMouse && hWindowUnderMouse != h) {
                            SendMessageW(hWindowUnderMouse, msg, wParam, lParam);
                            break;
                        }

                        event.type = MouseEvent::Type::SCROLL_VERT;
                        event.scrollDelta = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;
                        break;

                    case WM_MOUSELEAVE:
                        event.type = MouseEvent::Type::LEAVE;
                        break;
                    case WM_MOUSEMOVE: {
                        event.type = MouseEvent::Type::MOTION;

                        if(wParam & MK_LBUTTON) {
                            event.button = MouseEvent::Button::LEFT;
                        } else if(wParam & MK_MBUTTON) {
                            event.button = MouseEvent::Button::MIDDLE;
                        } else if(wParam & MK_RBUTTON) {
                            event.button = MouseEvent::Button::RIGHT;
                        }

                        // We need this in order to get the WM_MOUSELEAVE
                        TRACKMOUSEEVENT tme = {};
                        tme.cbSize    = sizeof(tme);
                        tme.dwFlags   = TME_LEAVE;
                        tme.hwndTrack = window->hWindow;
                        sscheck(TrackMouseEvent(&tme));
                        break;
                    }
                }

                if(window->onMouseEvent) {
                    window->onMouseEvent(event);
                }
                break;
            }

            case WM_KEYDOWN:
            case WM_KEYUP: {
                Platform::KeyboardEvent event = {};
                if(msg == WM_KEYDOWN) {
                    event.type = Platform::KeyboardEvent::Type::PRESS;
                } else if(msg == WM_KEYUP) {
                    event.type = Platform::KeyboardEvent::Type::RELEASE;
                }

                if(GetKeyState(VK_SHIFT) & 0x8000)
                    event.shiftDown = true;
                if(GetKeyState(VK_CONTROL) & 0x8000)
                    event.controlDown = true;

                if(wParam >= VK_F1 && wParam <= VK_F12) {
                    event.key = Platform::KeyboardEvent::Key::FUNCTION;
                    event.num = wParam - VK_F1 + 1;
                } else {
                    event.key = Platform::KeyboardEvent::Key::CHARACTER;
                    event.chr = tolower(MapVirtualKeyW(wParam, MAPVK_VK_TO_CHAR));
                    if(event.chr == 0) {
                        if(wParam == VK_DELETE) {
                            event.chr = '\x7f';
                        } else {
                            // Non-mappable key.
                            break;
                        }
                    } else if(event.chr == '.' && event.shiftDown) {
                        event.chr = '>';
                        event.shiftDown = false;;
                    }
                }

                if(window->onKeyboardEvent) {
                    window->onKeyboardEvent(event);
                } else {
                    HWND hParent;
                    sscheck(hParent = GetParent(h));
                    if(hParent != NULL) {
                        sscheck(SetForegroundWindow(hParent));
                        sscheck(SendMessageW(hParent, msg, wParam, lParam));
                    }
                }
                break;
            }

            case WM_SYSKEYDOWN: {
                HWND hParent;
                sscheck(hParent = GetParent(h));
                if(hParent != NULL) {
                    // If the user presses the Alt key when a tool window has focus,
                    // then that should probably go to the main window instead.
                    sscheck(SetForegroundWindow(hParent));
                    break;
                } else {
                    return DefWindowProc(h, msg, wParam, lParam);
                }
            }

            case WM_VSCROLL: {
                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask  = SIF_POS|SIF_TRACKPOS|SIF_RANGE|SIF_PAGE;
                sscheck(GetScrollInfo(window->hWindow, SB_VERT, &si));

                switch(LOWORD(wParam)) {
                    case SB_LINEUP:         si.nPos -= SCROLLBAR_UNIT; break;
                    case SB_PAGEUP:         si.nPos -= si.nPage;       break;
                    case SB_LINEDOWN:       si.nPos += SCROLLBAR_UNIT; break;
                    case SB_PAGEDOWN:       si.nPos += si.nPage;       break;
                    case SB_TOP:            si.nPos  = si.nMin;        break;
                    case SB_BOTTOM:         si.nPos  = si.nMax;        break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION:  si.nPos  = si.nTrackPos;   break;
                }

                si.nPos = min((UINT)si.nPos, (UINT)(si.nMax - si.nPage));

                if(window->onScrollbarAdjusted) {
                    window->onScrollbarAdjusted((double)si.nPos / SCROLLBAR_UNIT);
                }
                break;
            }

            case WM_MENUCOMMAND: {
                MENUITEMINFOW mii = {};
                mii.cbSize = sizeof(mii);
                mii.fMask  = MIIM_DATA;
                sscheck(GetMenuItemInfoW((HMENU)lParam, wParam, TRUE, &mii));

                MenuItemImplWin32 *menuItem = (MenuItemImplWin32 *)mii.dwItemData;
                if(menuItem->onTrigger) {
                    menuItem->onTrigger();
                }
                break;
            }

            default:
                return DefWindowProc(h, msg, wParam, lParam);
        }

        return 0;
    }

    static LRESULT CALLBACK EditorWndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
        if(handlingFatalError) return 0;

        HWND hWindow;
        sscheck(hWindow = GetParent(h));

        WindowImplWin32 *window;
        sscheck(window = (WindowImplWin32 *)GetWindowLongPtr(hWindow, 0));

        switch(msg) {
            case WM_KEYDOWN:
                if(wParam == VK_RETURN) {
                    if(window->onEditingDone) {
                        int length;
                        sscheck(length = GetWindowTextLength(h));

                        std::wstring resultW;
                        resultW.resize(length);
                        sscheck(GetWindowTextW(h, &resultW[0], resultW.length() + 1));

                        window->onEditingDone(Narrow(resultW));
                        return 0;
                    }
                } else if(wParam == VK_ESCAPE) {
                    sscheck(SendMessageW(hWindow, msg, wParam, lParam));
                    return 0;
                }
        }

        return CallWindowProc(window->editorWndProc, h, msg, wParam, lParam);
    }

    double GetPixelDensity() override {
        UINT dpi;
        sscheck(dpi = ssGetDpiForWindow(hWindow));
        return (double)dpi;
    }

    int GetDevicePixelRatio() override {
        UINT dpi;
        sscheck(dpi = ssGetDpiForWindow(hWindow));
        return dpi / USER_DEFAULT_SCREEN_DPI;
    }

    bool IsVisible() override {
        BOOL isVisible;
        sscheck(isVisible = IsWindowVisible(hWindow));
        return isVisible == TRUE;
    }

    void SetVisible(bool visible) override {
        sscheck(ShowWindow(hWindow, visible ? SW_SHOW : SW_HIDE));
    }

    void Focus() override {
        sscheck(SetActiveWindow(hWindow));
    }

    bool IsFullScreen() override {
        DWORD style;
        sscheck(style = GetWindowLongPtr(hWindow, GWL_STYLE));
        return !(style & WS_OVERLAPPEDWINDOW);
    }

    void SetFullScreen(bool fullScreen) override {
        DWORD style;
        sscheck(style = GetWindowLongPtr(hWindow, GWL_STYLE));
        if(fullScreen) {
            sscheck(GetWindowPlacement(hWindow, &placement));

            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            sscheck(GetMonitorInfo(MonitorFromWindow(hWindow, MONITOR_DEFAULTTONEAREST), &mi));

            sscheck(SetWindowLong(hWindow, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW));
            sscheck(SetWindowPos(hWindow, HWND_TOP,
                                 mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left,
                                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_NOOWNERZORDER|SWP_FRAMECHANGED));
        } else {
            sscheck(SetWindowLong(hWindow, GWL_STYLE, style | WS_OVERLAPPEDWINDOW));
            sscheck(SetWindowPlacement(hWindow, &placement));
            sscheck(SetWindowPos(hWindow, NULL, 0, 0, 0, 0,
                                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|
                                 SWP_NOOWNERZORDER|SWP_FRAMECHANGED));
        }
    }

    void SetTitle(const std::string &title) override {
        sscheck(SetWindowTextW(hWindow, Title(title).c_str()));
    }

    void SetMenuBar(MenuBarRef newMenuBar) override {
        menuBar = std::static_pointer_cast<MenuBarImplWin32>(newMenuBar);

        MENUINFO mi = {};
        mi.cbSize  = sizeof(mi);
        mi.fMask   = MIM_APPLYTOSUBMENUS|MIM_STYLE;
        mi.dwStyle = MNS_NOTIFYBYPOS;
        sscheck(SetMenuInfo(menuBar->hMenuBar, &mi));

        sscheck(SetMenu(hWindow, menuBar->hMenuBar));
    }

    void GetContentSize(double *width, double *height) override {
        int pixelRatio = GetDevicePixelRatio();

        RECT rc;
        sscheck(GetClientRect(hWindow, &rc));
        *width  = (rc.right  - rc.left) / pixelRatio;
        *height = (rc.bottom - rc.top)  / pixelRatio;
    }

    void SetMinContentSize(double width, double height) {
        minWidth  = (int)width;
        minHeight = (int)height;

        int pixelRatio = GetDevicePixelRatio();

        RECT rc;
        sscheck(GetClientRect(hWindow, &rc));
        if(rc.right  - rc.left < minWidth * pixelRatio) {
            rc.right  = rc.left + minWidth  * pixelRatio;
        }
        if(rc.bottom - rc.top < minHeight * pixelRatio) {
            rc.bottom = rc.top  + minHeight * pixelRatio;
        }
    }

    void FreezePosition(const std::string &key) override {
        sscheck(GetWindowPlacement(hWindow, &placement));

        BOOL isMaximized;
        sscheck(isMaximized = IsZoomed(hWindow));

        RECT rc = placement.rcNormalPosition;
        CnfFreezeInt(rc.left,     key + "_left");
        CnfFreezeInt(rc.right,    key + "_right");
        CnfFreezeInt(rc.top,      key + "_top");
        CnfFreezeInt(rc.bottom,   key + "_bottom");
        CnfFreezeInt(isMaximized, key + "_maximized");
    }

    void ThawPosition(const std::string &key) override {
        sscheck(GetWindowPlacement(hWindow, &placement));

        RECT rc = placement.rcNormalPosition;
        rc.left   = CnfThawInt(rc.left,   key + "_left");
        rc.right  = CnfThawInt(rc.right,  key + "_right");
        rc.top    = CnfThawInt(rc.top,    key + "_top");
        rc.bottom = CnfThawInt(rc.bottom, key + "_bottom");

        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        sscheck(GetMonitorInfo(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi));

        // If it somehow ended up off-screen, then put it back.
        RECT mrc = mi.rcMonitor;
        rc.left   = Clamp(rc.left,   mrc.left, mrc.right);
        rc.right  = Clamp(rc.right,  mrc.left, mrc.right);
        rc.top    = Clamp(rc.top,    mrc.top,  mrc.bottom);
        rc.bottom = Clamp(rc.bottom, mrc.top,  mrc.bottom);

        // And make sure the minimum size is respected. (We can freeze a size smaller
        // than minimum size if the DPI changed between runs.)
        sscheck(SendMessageW(hWindow, WM_SIZING, WMSZ_BOTTOMRIGHT, (LPARAM)&rc));

        placement.flags = 0;
        if(CnfThawInt(false, key + "_maximized")) {
            placement.showCmd = SW_SHOWMAXIMIZED;
        } else {
            placement.showCmd = SW_SHOW;
        }
        placement.rcNormalPosition = rc;
        sscheck(SetWindowPlacement(hWindow, &placement));
    }

    void SetCursor(Cursor cursor) override {
        LPWSTR cursorName;
        switch(cursor) {
            case Cursor::POINTER: cursorName = IDC_ARROW; break;
            case Cursor::HAND:    cursorName = IDC_HAND;  break;
        }

        HCURSOR hCursor;
        sscheck(hCursor = LoadCursorW(NULL, cursorName));
        sscheck(::SetCursor(hCursor));
    }

    void SetTooltip(const std::string &newText) override {
        // The following SendMessage calls sometimes fail with ERROR_ACCESS_DENIED for
        // no discernible reason, but only on wine.
        if(newText.empty()) {
            SendMessageW(hTooltip, TTM_ACTIVATE, FALSE, 0);
            SendMessageW(hTooltip, TTM_POP, 0, 0);
        } else if(newText != tooltipText) {
            tooltipText = newText;

            std::wstring newTextW = Widen(newText);
            TOOLINFOW ti = {};
            ti.cbSize   = sizeof(ti);
            ti.uFlags   = TTF_IDISHWND;
            ti.hwnd     = hWindow;
            ti.uId      = (UINT_PTR)hWindow;
            ti.lpszText = &newTextW[0];
            SendMessageW(hTooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

            SendMessageW(hTooltip, TTM_ACTIVATE, TRUE, 0);
            SendMessageW(hTooltip, TTM_POPUP, 0, 0);
        }
    }

    bool IsEditorVisible() override {
        BOOL visible;
        sscheck(visible = IsWindowVisible(hEditor));
        return visible == TRUE;
    }

    void ShowEditor(double x, double y, double fontHeight, double minWidth,
                    bool isMonospace, const std::string &text) override {
        if(IsEditorVisible()) return;

        int pixelRatio = GetDevicePixelRatio();

        HFONT hFont = CreateFontW(-(LONG)fontHeight * GetDevicePixelRatio(), 0, 0, 0,
            FW_REGULAR, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FF_DONTCARE, isMonospace ? L"Lucida Console" : L"Arial");
        if(hFont == NULL) {
            sscheck(hFont = (HFONT)GetStockObject(SYSTEM_FONT));
        }
        sscheck(SendMessageW(hEditor, WM_SETFONT, (WPARAM)hFont, FALSE));
        sscheck(SendMessageW(hEditor, EM_SETMARGINS, EC_LEFTMARGIN|EC_RIGHTMARGIN, 0));

        std::wstring textW = Widen(text);

        HDC hDc;
        TEXTMETRICW tm;
        SIZE ts;
        sscheck(hDc = GetDC(hEditor));
        sscheck(SelectObject(hDc, hFont));
        sscheck(GetTextMetricsW(hDc, &tm));
        sscheck(GetTextExtentPoint32W(hDc, textW.c_str(), textW.length(), &ts));
        sscheck(ReleaseDC(hEditor, hDc));

        RECT rc;
        rc.left   = (LONG)x * pixelRatio;
        rc.top    = (LONG)y * pixelRatio - tm.tmAscent;
        // Add one extra char width to avoid scrolling.
        rc.right  = (LONG)x * pixelRatio +
                    std::max((LONG)minWidth * pixelRatio, ts.cx + tm.tmAveCharWidth);
        rc.bottom = (LONG)y * pixelRatio + tm.tmDescent;
        sscheck(ssAdjustWindowRectExForDpi(&rc, 0, /*bMenu=*/FALSE, WS_EX_CLIENTEDGE,
			                               ssGetDpiForWindow(hWindow)));

        sscheck(MoveWindow(hEditor, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                           /*bRepaint=*/true));
        sscheck(ShowWindow(hEditor, SW_SHOW));
        if(!textW.empty()) {
            sscheck(SendMessageW(hEditor, WM_SETTEXT, 0, (LPARAM)textW.c_str()));
            sscheck(SendMessageW(hEditor, EM_SETSEL, 0, textW.length()));
            sscheck(SetFocus(hEditor));
        }
    }

    void HideEditor() override {
        if(!IsEditorVisible()) return;

        sscheck(ShowWindow(hEditor, SW_HIDE));
    }

    void SetScrollbarVisible(bool visible) override {
        scrollbarVisible = visible;
        sscheck(ShowScrollBar(hWindow, SB_VERT, visible));
    }

    void ConfigureScrollbar(double min, double max, double pageSize) override {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE|SIF_PAGE;
        si.nMin   = (UINT)(min * SCROLLBAR_UNIT);
        si.nMax   = (UINT)(max * SCROLLBAR_UNIT);
        si.nPage  = (UINT)(pageSize * SCROLLBAR_UNIT);
        sscheck(SetScrollInfo(hWindow, SB_VERT, &si, /*redraw=*/TRUE));
    }

    double GetScrollbarPosition() override {
        if(!scrollbarVisible) return 0.0;

        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        sscheck(GetScrollInfo(hWindow, SB_VERT, &si));
        return (double)si.nPos / SCROLLBAR_UNIT;
    }

    void SetScrollbarPosition(double pos) override {
        if(!scrollbarVisible) return;

        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = (UINT)(pos * SCROLLBAR_UNIT);
        sscheck(SetScrollInfo(hWindow, SB_VERT, &si, /*redraw=*/TRUE));

        // Windows won't synthesize a WM_VSCROLL for us here.
        if(onScrollbarAdjusted) {
            onScrollbarAdjusted((double)si.nPos / SCROLLBAR_UNIT);
        }
    }

    void Invalidate() override {
        sscheck(InvalidateRect(hWindow, NULL, /*bErase=*/FALSE));
    }

    void Redraw() override {
        Invalidate();
        sscheck(UpdateWindow(hWindow));
    }

    void *NativePtr() override {
        return hWindow;
    }
};

WindowRef CreateWindow(Window::Kind kind, WindowRef parentWindow) {
    return std::make_shared<WindowImplWin32>(kind,
                std::static_pointer_cast<WindowImplWin32>(parentWindow));
}

//-----------------------------------------------------------------------------
// Application-wide APIs
//-----------------------------------------------------------------------------

void Exit() {
    PostQuitMessage(0);
}

}
}