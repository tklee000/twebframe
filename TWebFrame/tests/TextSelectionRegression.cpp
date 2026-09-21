#include <TWebFrame/TWebFrame.h>

#include <windows.h>
#include <windowsx.h>
#include <ole2.h>

#include <array>
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

std::wstring ClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return {};
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    const std::wstring result = text ? text : L"";
    if (text) GlobalUnlock(data);
    CloseClipboard();
    return result;
}

void SendControlKey(HWND window, WPARAM key) {
    std::array<BYTE, 256> previous{};
    GetKeyboardState(previous.data());
    auto pressed = previous;
    pressed[VK_CONTROL] |= 0x80;
    pressed[VK_LCONTROL] |= 0x80;
    SetKeyboardState(pressed.data());
    SendMessageW(window, WM_KEYDOWN, key, 0);
    SetKeyboardState(previous.data());
}

UINT CheckPointerSelection(DPI_AWARENESS_CONTEXT context) {
    const auto previous = SetThreadDpiAwarenessContext(context);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP | WS_VISIBLE,
                                -10000, -10000, 900, 140, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Check(host != nullptr, L"the off-screen selection host is created");
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (host) {
        dpi = GetDpiForWindow(host);
        RECT bounds{0, 0, 900, 140};
        auto view = TWebFrame::View::Create(host, bounds);
        Check(view != nullptr, L"the text-selection view is created");
        if (view) {
            constexpr wchar_t initialValue[] =
                L"D:\\FA50Data\\DataSamples\\2026-05-29_14_20_28_KST_ch4.info";
            const wchar_t* html = LR"HTML(
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    input { position: absolute; left: 20px; top: 20px; width: 760px;
                            height: 44px; padding: 6px 10px; border: 1px solid #888;
                            font: 20px Consolas; }
                </style>
                <input type="text" value="D:\FA50Data\DataSamples\2026-05-29_14_20_28_KST_ch4.info">
            )HTML";
            Check(view->NavigateToString(html), L"the generic path input fixture loads");

            std::wstring geometry, error;
            Check(view->ExecuteScript(
                      L"const r=document.querySelector('input').getBoundingClientRect();"
                      L"return Math.round(r.x+12)+','+Math.round(r.x+310)+','+"
                      L"Math.round(r.y+r.height/2);",
                      &geometry, &error), error.c_str());
            const size_t firstComma = geometry.find(L',');
            const size_t secondComma = geometry.find(L',', firstComma + 1);
            Check(firstComma != std::wstring::npos && secondComma != std::wstring::npos,
                  L"input drag coordinates are available");
            if (firstComma != std::wstring::npos && secondComma != std::wstring::npos) {
                const double scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
                const auto physical = [scale](const std::wstring& value) {
                    return static_cast<int>(std::lround(std::stod(value) * scale));
                };
                const int startX = physical(geometry.substr(0, firstComma));
                const int endX = physical(geometry.substr(firstComma + 1,
                                                           secondComma - firstComma - 1));
                const int y = physical(geometry.substr(secondComma + 1));

                const HWND window = view->Window();
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(startX, y));
                Check(GetCapture() == window, L"text selection captures the pointer while dragging");
                SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(endX, y));
                SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(endX, y));
                Check(GetCapture() != window, L"text selection releases pointer capture on button up");

                std::wstring selected;
                Check(view->ExecuteScript(
                          L"const e=document.querySelector('input');"
                          L"return e.selectionStart+'|'+e.selectionEnd;",
                          &selected, &error), error.c_str());
                const size_t firstBar = selected.find(L'|');
                const size_t selectionStart = firstBar == std::wstring::npos
                    ? 0 : std::stoull(selected.substr(0, firstBar));
                const size_t selectionEnd = firstBar == std::wstring::npos
                    ? 0 : std::stoull(selected.substr(firstBar + 1));
                const std::wstring selectedText = selectionEnd > selectionStart
                    ? std::wstring(initialValue).substr(selectionStart, selectionEnd - selectionStart)
                    : L"";
                Check(firstBar != std::wstring::npos && selectionEnd > selectionStart &&
                          !selectedText.empty(),
                      L"pointer dragging creates a non-empty DOM text selection");

                SendControlKey(window, L'C');
                const auto copiedText = ClipboardText(window);
                Check(!selectedText.empty() && copiedText == selectedText,
                      L"Ctrl+C copies the dragged selection through the common clipboard path");

                Check(view->ExecuteScript(
                          L"const e=document.querySelector('input');"
                          L"e.setSelectionRange(e.value.length,e.value.length);",
                          nullptr, &error), error.c_str());
                SendControlKey(window, L'V');
                std::wstring pasted;
                Check(view->ExecuteScript(
                          L"return document.querySelector('input').value;", &pasted, &error),
                      error.c_str());
                Check(pasted == std::wstring(initialValue) + selectedText,
                      L"Ctrl+V pastes the copied drag selection into the input");

                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(endX, y));
                SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(startX, y));
                SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(startX, y));
                std::wstring reverseSelection;
                Check(view->ExecuteScript(
                          L"const e=document.querySelector('input');"
                          L"return e.selectionStart+'|'+e.selectionEnd;",
                          &reverseSelection, &error), error.c_str());
                const size_t reverseBar = reverseSelection.find(L'|');
                Check(reverseBar != std::wstring::npos &&
                          std::stoull(reverseSelection.substr(reverseBar + 1)) >
                              std::stoull(reverseSelection.substr(0, reverseBar)),
                      L"right-to-left dragging exposes normalized DOM selection bounds");
            }
        }
        DestroyWindow(host);
    }
    SetThreadDpiAwarenessContext(previous);
    return dpi;
}

} // namespace

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const UINT dpi100 = CheckPointerSelection(DPI_AWARENESS_CONTEXT_UNAWARE);
    const UINT monitorDpi = CheckPointerSelection(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(dpi100 == USER_DEFAULT_SCREEN_DPI,
          L"the DPI-unaware regression path uses 100 percent coordinates");
    Check(monitorDpi >= USER_DEFAULT_SCREEN_DPI,
          L"the per-monitor regression path uses the monitor DPI coordinates");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Text selection regression tests passed at 100% and monitor DPI ("
               << monitorDpi << L" DPI)\n";
    return 0;
}
