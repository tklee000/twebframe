#include <TWebFrame/TWebFrame.h>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <ole2.h>

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

struct TooltipSearch {
    HWND owner = nullptr;
    HWND result = nullptr;
};

BOOL CALLBACK FindTooltipWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<TooltipSearch*>(parameter);
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, TOOLTIPS_CLASSW) == 0 &&
        GetWindow(window, GW_OWNER) == GetAncestor(search.owner, GA_ROOT)) {
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        if (SendMessageW(window, TTM_GETTOOLCOUNT, 0, 0) == 1 &&
            SendMessageW(window, TTM_ENUMTOOLSW, 0,
                         reinterpret_cast<LPARAM>(&tool)) && tool.hwnd == search.owner) {
            search.result = window;
            return FALSE;
        }
    }
    return TRUE;
}

HWND TooltipFor(HWND owner) {
    TooltipSearch search{owner, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), FindTooltipWindow,
                      reinterpret_cast<LPARAM>(&search));
    return search.result;
}

std::wstring TooltipText(HWND tooltip) {
    if (!tooltip) return {};
    TOOLINFOW current{};
    current.cbSize = TTTOOLINFOW_V2_SIZE;
    if (!SendMessageW(tooltip, TTM_GETCURRENTTOOLW, 0,
                      reinterpret_cast<LPARAM>(&current))) return {};
    wchar_t text[512]{};
    current.lpszText = text;
    SendMessageW(tooltip, TTM_GETTEXTW, 0, reinterpret_cast<LPARAM>(&current));
    return text;
}

LPARAM PointerPosition(float cssX, float cssY, float scale) {
    return MAKELPARAM(static_cast<int>(std::lround(cssX * scale)),
                      static_cast<int>(std::lround(cssY * scale)));
}

void Hover(HWND window, float cssX, float cssY, float scale) {
    const LPARAM position = PointerPosition(cssX, cssY, scale);
    SendMessageW(window, WM_MOUSEMOVE, 0, position);
    // The OS normally sends this after its configured hover delay. Sending the
    // resulting message directly keeps the regression deterministic.
    SendMessageW(window, WM_MOUSEHOVER, 0, position);
}

UINT CheckTitleTooltips(DPI_AWARENESS_CONTEXT context) {
    const auto previous = SetThreadDpiAwarenessContext(context);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"",
                                WS_POPUP | WS_VISIBLE, workArea.left + 40, workArea.top + 40,
                                600, 300,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(host != nullptr, L"the tooltip regression host is created");
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (host) {
        dpi = GetDpiForWindow(host);
        const float scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
        RECT bounds{0, 0, static_cast<LONG>(std::lround(300.0f * scale)),
                          static_cast<LONG>(std::lround(150.0f * scale))};
        auto view = TWebFrame::View::Create(host, bounds);
        Check(view != nullptr, L"the tooltip regression view is created");
        if (view) {
            const wchar_t* html = LR"HTML(
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    .row { display: block; width: 240px; height: 40px; }
                    .fill { display: block; width: 100%; height: 100%; }
                </style>
                <div class="row" title="Inherited title"><span class="fill">one</span></div>
                <div class="row" title="Blocked title"><span class="fill" title="">two</span></div>
                <div class="row" id="dynamic" title="Initial title"><span class="fill">three</span></div>
            )HTML";
            Check(view->NavigateToString(html), L"the generic title fixture loads");
            const HWND window = view->Window();
            const HWND tooltip = TooltipFor(window);
            Check(tooltip != nullptr, L"the view owns one common tooltip surface");

            Hover(window, 10.0f, 10.0f, scale);
            Check(tooltip && IsWindowVisible(tooltip),
                  L"an inherited title becomes visible after hover");
            Check(TooltipText(tooltip) == L"Inherited title",
                  L"title advisory text is inherited through nested DOM content");
            Check(!tooltip || GetDpiForWindow(tooltip) == GetDpiForWindow(window),
                  L"the tooltip and DOM surface use the same effective DPI");
            if (tooltip) {
                RECT tooltipRect{};
                GetWindowRect(tooltip, &tooltipRect);
                POINT expected{static_cast<LONG>(std::lround(10.0f * scale)),
                               static_cast<LONG>(std::lround(10.0f * scale))};
                ClientToScreen(window, &expected);
                expected.y += GetSystemMetricsForDpi(SM_CYCURSOR, dpi) / 2;
                Check(std::abs(tooltipRect.left - expected.x) <= 1 &&
                          std::abs(tooltipRect.top - expected.y) <= 1,
                      L"the tracked tooltip uses the hovered element's screen position");
                Check(SendMessageW(tooltip, TTM_GETTIPBKCOLOR, 0, 0) == RGB(255, 255, 255) &&
                          SendMessageW(tooltip, TTM_GETTIPTEXTCOLOR, 0, 0) == RGB(0, 0, 0),
                      L"title tooltips use the browser-compatible neutral palette");
                RECT margin{};
                SendMessageW(tooltip, TTM_GETMARGIN, 0,
                             reinterpret_cast<LPARAM>(&margin));
                Check(margin.left == static_cast<LONG>(std::lround(6.0f * scale)) &&
                          margin.top == static_cast<LONG>(std::lround(3.0f * scale)),
                      L"title tooltip padding follows the active DPI");
            }

            Hover(window, 10.0f, 50.0f, scale);
            Check(!tooltip || !IsWindowVisible(tooltip),
                  L"an explicitly empty title suppresses an ancestor title");

            Hover(window, 10.0f, 90.0f, scale);
            Check(tooltip && IsWindowVisible(tooltip) &&
                      TooltipText(tooltip) == L"Initial title",
                  L"a directly declared title is shown");

            std::wstring error;
            Check(view->ExecuteScript(
                      L"document.getElementById('dynamic').title='Updated title';",
                      nullptr, &error), error.c_str());
            SendMessageW(window, WM_MOUSEHOVER, 0,
                         PointerPosition(10.0f, 90.0f, scale));
            Check(tooltip && IsWindowVisible(tooltip) &&
                      TooltipText(tooltip) == L"Updated title",
                  L"JavaScript title-property changes update the common tooltip");

            Check(view->ExecuteScript(
                      L"document.getElementById('dynamic').removeAttribute('title');",
                      nullptr, &error), error.c_str());
            Check(!tooltip || !IsWindowVisible(tooltip),
                  L"removing a hovered title immediately closes its tooltip");

            SendMessageW(window, WM_MOUSELEAVE, 0, 0);
        }
        DestroyWindow(host);
    }
    SetThreadDpiAwarenessContext(previous);
    return dpi;
}

} // namespace

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const UINT dpi100 = CheckTitleTooltips(DPI_AWARENESS_CONTEXT_UNAWARE);
    const UINT monitorDpi = CheckTitleTooltips(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(dpi100 == USER_DEFAULT_SCREEN_DPI,
          L"the DPI-unaware tooltip path uses 100 percent coordinates");
    Check(monitorDpi >= USER_DEFAULT_SCREEN_DPI,
          L"the per-monitor tooltip path uses the monitor DPI coordinates");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Tooltip regression tests passed at 100% and monitor DPI ("
               << monitorDpi << L" DPI)\n";
    return 0;
}
