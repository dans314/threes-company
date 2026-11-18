/*
 * Three's Company - the plugin window on Windows: a WebView2 (Microsoft Edge) view inside the
 * host's window. The same page as on macOS; only this file is Windows code.
 *
 * Needs the WebView2 Runtime. It is part of Windows 11 and has been rolled out to Windows 10 since
 * 2022; if it is missing, the window says so instead of staying blank.
 * WebView2.h and WebView2LoaderStatic.lib come from the Microsoft.Web.WebView2 NuGet package
 * (the GitHub build downloads it).
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl.h>
#include <string>

#include "WebView2.h"
#include "tc_gui.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
struct WinGui
{
    tcgui::Bridge *bridge{nullptr};
    HWND hwnd{nullptr};
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> web;
    int w{1280}, h{940};
    bool visible{true};
    bool failed{false};
    bool dead{false};
};

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const wchar_t *w)
{
    if (!w)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// WebView2 keeps a small cache; it must be somewhere writable (not next to the host's .exe)
std::wstring dataFolder()
{
    wchar_t *base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
    {
        dir = std::wstring(base) + L"\\Threes Company";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\WebView2";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    if (base)
        CoTaskMemFree(base);
    return dir;
}

const wchar_t *kClass = L"ThreesCompanyWebHost";

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto *g = (WinGui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_TIMER:
        if (g && g->web && !g->dead)
        {
            std::string js = g->bridge->poll();
            if (!js.empty())
                g->web->ExecuteScript(widen(js).c_str(), nullptr);
        }
        return 0;
    case WM_SIZE:
        if (g && g->ctl)
        {
            RECT r;
            GetClientRect(hwnd, &r);
            g->ctl->put_Bounds(r);
        }
        return 0;
    case WM_PAINT:
        if (g && g->failed)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
            InflateRect(&r, -24, -24);
            DrawTextW(dc,
                      L"Three's Company needs the Microsoft Edge WebView2 Runtime to show its window.\r\n\r\n"
                      L"Install it from https://developer.microsoft.com/microsoft-edge/webview2/ "
                      L"(the \"Evergreen Bootstrapper\"), then open the plugin again.\r\n\r\n"
                      L"The sound works without it; the host's own controls still change every setting.",
                      -1, &r, DT_WORDBREAK | DT_LEFT);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClass()
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

void startWebView(WinGui *g)
{
    std::wstring dir = dataFolder();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dir.empty() ? nullptr : dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [g](HRESULT res, ICoreWebView2Environment *env) -> HRESULT
            {
                if (g->dead)
                    return S_OK;
                if (FAILED(res) || !env)
                {
                    g->failed = true;
                    InvalidateRect(g->hwnd, nullptr, TRUE);
                    return S_OK;
                }
                return env->CreateCoreWebView2Controller(
                    g->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [g](HRESULT res2, ICoreWebView2Controller *c) -> HRESULT
                                 {
                                     if (g->dead)
                                     {
                                         if (c)
                                             c->Close();
                                         return S_OK;
                                     }
                                     if (FAILED(res2) || !c)
                                     {
                                         g->failed = true;
                                         InvalidateRect(g->hwnd, nullptr, TRUE);
                                         return S_OK;
                                     }
                                     g->ctl = c;
                                     g->ctl->get_CoreWebView2(&g->web);
                                     RECT r;
                                     GetClientRect(g->hwnd, &r);
                                     g->ctl->put_Bounds(r);
                                     g->ctl->put_IsVisible(g->visible ? TRUE : FALSE);
                                     ComPtr<ICoreWebView2Settings> st;
                                     if (SUCCEEDED(g->web->get_Settings(&st)) && st)
                                     {
                                         st->put_AreDevToolsEnabled(FALSE);
                                         st->put_IsStatusBarEnabled(FALSE);
                                         st->put_IsZoomControlEnabled(FALSE);
                                         st->put_AreDefaultContextMenusEnabled(FALSE); // the page has its own right-click menus
                                     }
                                     EventRegistrationToken tok;
                                     g->web->add_WebMessageReceived(
                                         Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                             [g](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT
                                             {
                                                 LPWSTR m = nullptr;
                                                 if (!g->dead && SUCCEEDED(a->TryGetWebMessageAsString(&m)) && m)
                                                     g->bridge->onMessage(narrow(m).c_str());
                                                 if (m)
                                                     CoTaskMemFree(m);
                                                 return S_OK;
                                             })
                                             .Get(),
                                         &tok);
                                     g->web->NavigateToString(widen(g->bridge->html()).c_str());
                                     SetTimer(g->hwnd, 1, 33, nullptr); // ~30 updates a second, as on macOS
                                     return S_OK;
                                 })
                                 .Get());
            })
            .Get());
    if (FAILED(hr))
    {
        g->failed = true;
        InvalidateRect(g->hwnd, nullptr, TRUE);
    }
}
} // namespace

namespace tcgui
{
void *create(Bridge *b, int w, int h)
{
    auto *g = new WinGui();
    g->bridge = b, g->w = w, g->h = h;
    return g;
}

bool setParent(void *gui, void *parent)
{
    auto *g = (WinGui *)gui;
    if (!g || !parent)
        return false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the host's UI thread usually has COM already
    registerClass();
    g->hwnd = CreateWindowExW(0, kClass, L"Three's Company", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, g->w, g->h,
                              (HWND)parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g->hwnd)
        return false;
    SetWindowLongPtrW(g->hwnd, GWLP_USERDATA, (LONG_PTR)g);
    startWebView(g);
    return true;
}

void setSize(void *gui, int w, int h)
{
    auto *g = (WinGui *)gui;
    if (!g)
        return;
    g->w = w, g->h = h;
    if (g->hwnd)
        SetWindowPos(g->hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void show(void *gui, bool on)
{
    auto *g = (WinGui *)gui;
    if (!g)
        return;
    g->visible = on;
    if (g->hwnd)
        ShowWindow(g->hwnd, on ? SW_SHOW : SW_HIDE);
    if (g->ctl)
        g->ctl->put_IsVisible(on ? TRUE : FALSE);
}

void destroy(void *gui)
{
    auto *g = (WinGui *)gui;
    if (!g)
        return;
    g->dead = true;
    if (g->hwnd)
        KillTimer(g->hwnd, 1);
    if (g->ctl)
        g->ctl->Close();
    g->web.Reset();
    g->ctl.Reset();
    if (g->hwnd)
    {
        SetWindowLongPtrW(g->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(g->hwnd);
    }
    // callbacks still in flight hold a pointer to g; it is leaked on purpose (a few bytes) rather than freed under them
}

std::string chooseFolder(void *gui, const std::string &start)
{
    auto *g = (WinGui *)gui;
    std::string out;
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d)
        return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Choose where Three's Company keeps your presets");
    d->SetOkButtonLabel(L"Use This Folder");
    if (!start.empty())
    {
        IShellItem *at = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(widen(start).c_str(), nullptr, IID_PPV_ARGS(&at))) && at)
        {
            d->SetFolder(at);
            at->Release();
        }
    }
    HWND owner = g && g->hwnd ? GetAncestor(g->hwnd, GA_ROOT) : nullptr;
    if (SUCCEEDED(d->Show(owner)))
    {
        IShellItem *r = nullptr;
        if (SUCCEEDED(d->GetResult(&r)) && r)
        {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
            {
                out = narrow(p);
                CoTaskMemFree(p);
            }
            r->Release();
        }
    }
    d->Release();
    return out;
}

void reveal(const std::string &path)
{
    if (!path.empty())
        ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // namespace tcgui
