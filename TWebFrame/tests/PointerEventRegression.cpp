#include <TWebFrame/TWebFrame.h>

#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <windows.h>
#include <ole2.h>

#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace TWebFrame::Internal;

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

LPARAM PointerPosition(float cssX, float cssY, float scale) {
    return MAKELPARAM(static_cast<int>(std::lround(cssX * scale)),
                      static_cast<int>(std::lround(cssY * scale)));
}

bool ScriptEquals(TWebFrame::View& view, const wchar_t* script,
                  const wchar_t* expected, const wchar_t* message) {
    std::wstring result;
    std::wstring error;
    const bool executed = view.ExecuteScript(script, &result, &error);
    if (!executed && !error.empty()) std::wcerr << L"JavaScript: " << error << L'\n';
    const bool matches = executed && result == expected;
    Check(matches, message);
    if (executed && !matches)
        std::wcerr << L"  expected: " << expected << L"\n  actual:   " << result << L'\n';
    return matches;
}

void CheckCommonEngineScale(float scale) {
    const wchar_t* html = LR"HTML(
        <style>
            * { box-sizing: border-box; margin: 0; padding: 0; }
            #grid { position: relative; display: grid; grid-template-columns: repeat(3, 18px); gap: 5px; width: 64px; }
            #grid::after { content: ""; position: absolute; inset: 0; pointer-events: none; }
            #grid button { width: 18px; min-width: 18px; height: 18px; min-height: 18px; }
        </style>
        <div id="grid">
            <button id="b11" data-table-columns="1" data-table-rows="1"></button>
            <button id="b12" data-table-columns="2" data-table-rows="1"></button>
            <button id="b13" data-table-columns="3" data-table-rows="1"></button>
            <button id="b21" data-table-columns="1" data-table-rows="2"></button>
            <button id="b22" data-table-columns="2" data-table-rows="2"></button>
            <button id="b23" data-table-columns="3" data-table-rows="2"></button>
        </div>
        <div id="label">0 x 0</div>
        <script>
            window.lastClientX = 0;
            document.getElementById("grid").addEventListener("pointerover", (event) => {
                const cell = event.target.closest("button[data-table-columns]");
                if (!cell) return;
                window.lastClientX = event.clientX;
                document.getElementById("label").textContent =
                    cell.dataset.tableColumns + " x " + cell.dataset.tableRows;
            });
        </script>
    )HTML";
    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());
    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());
    LayoutEngine layout(document, styles);
    layout.Layout(240.0f, 120.0f, scale);
    JavaScriptRuntime javascript(document);
    javascript.SetDevicePixelRatio(scale);
    Check(javascript.Load(document.ScriptText(), &error), error.c_str());

    const float clientX = std::round(55.0f * scale) / scale;
    const float clientY = std::round(32.0f * scale) / scale;
    const auto target = layout.HitTest(clientX, clientY);
    Check(target && target->Attribute(L"id") == L"b23",
          L"the common layout hit test resolves the same grid cell at 100% and 150%");
    JavaScriptRuntime::EventInit pointer{};
    pointer.clientX = pointer.pageX = clientX;
    pointer.clientY = pointer.pageY = clientY;
    javascript.DispatchNodeEvent(target, L"pointerover", pointer);
    std::wstring result;
    Check(javascript.Execute(
              L"return document.getElementById('label').textContent+'|'+"
              L"(Math.abs(window.lastClientX-" + std::to_wstring(clientX) + L")<0.01)+'|' +"
              L"window.devicePixelRatio;",
              &result, &error) &&
              result == (scale == 1.5f ? L"3 x 2|true|1.5" : L"3 x 2|true|1"),
          L"delegated pointerover remains in CSS-pixel coordinates at 100% and 150%");
}

UINT CheckPointerGridAtDpi(DPI_AWARENESS_CONTEXT context) {
    const auto previousContext = SetThreadDpiAwarenessContext(context);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP | WS_VISIBLE,
                                workArea.left + 80, workArea.top + 80, 500, 300,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(host != nullptr, L"the pointer-event regression host is created");
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (host) {
        dpi = GetDpiForWindow(host);
        const float scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
        RECT bounds{0, 0, static_cast<LONG>(std::lround(240.0f * scale)),
                          static_cast<LONG>(std::lround(120.0f * scale))};
        auto view = TWebFrame::View::Create(host, bounds);
        Check(view != nullptr, L"the pointer-event regression view is created");
        if (view) {
            const wchar_t* html = LR"HTML(
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    #grid { position: relative; display: grid; grid-template-columns: repeat(3, 18px); gap: 5px; width: 64px; }
                    #grid::after { content: ""; position: absolute; inset: 0; pointer-events: none; }
                    #grid button { width: 18px; min-width: 18px; height: 18px; min-height: 18px; }
                    #grid button.is-selected { background: red; }
                </style>
                <div id="grid">
                    <button id="b11" data-table-columns="1" data-table-rows="1"></button>
                    <button id="b12" data-table-columns="2" data-table-rows="1"></button>
                    <button id="b13" data-table-columns="3" data-table-rows="1"></button>
                    <button id="b21" data-table-columns="1" data-table-rows="2"></button>
                    <button id="b22" data-table-columns="2" data-table-rows="2"></button>
                    <button id="b23" data-table-columns="3" data-table-rows="2"></button>
                </div>
                <div id="label">0 x 0</div>
                <script>
                    window.overCount = 0;
                    window.moveCount = 0;
                    window.gridEnterCount = 0;
                    window.lastTarget = "";
                    window.lastRelated = "";
                    window.lastClientX = 0;
                    window.lastClientY = 0;
                    window.lastMovementX = 0;
                    window.lastMovementY = 0;
                    const grid = document.getElementById("grid");
                    grid.addEventListener("pointerenter", () => { window.gridEnterCount += 1; });
                    grid.addEventListener("pointerover", (event) => {
                        const cell = event.target.closest("button[data-table-columns]");
                        if (!cell) return;
                        window.overCount += 1;
                        window.lastTarget = event.target.id;
                        window.lastRelated = event.relatedTarget ? event.relatedTarget.id : "";
                        window.lastClientX = event.clientX;
                        window.lastClientY = event.clientY;
                        const columns = Number(cell.dataset.tableColumns);
                        const rows = Number(cell.dataset.tableRows);
                        document.getElementById("label").textContent = columns + " x " + rows;
                        grid.style.setProperty("--table-selection-width", (columns * 18 + (columns - 1) * 5) + "px");
                        grid.style.setProperty("--table-selection-height", (rows * 18 + (rows - 1) * 5) + "px");
                        document.querySelectorAll("#grid button").forEach((button) => {
                            const selected = Number(button.dataset.tableColumns) <= columns &&
                                Number(button.dataset.tableRows) <= rows;
                            button.classList.toggle("is-selected", selected);
                        });
                    });
                    grid.addEventListener("pointermove", (event) => {
                        window.moveCount += 1;
                        window.lastMovementX = event.movementX;
                        window.lastMovementY = event.movementY;
                    });
                </script>
            )HTML";
            Check(view->NavigateToString(html), L"the generic pointer-driven grid fixture loads");
            const HWND window = view->Window();
            UpdateWindow(window);

            SendMessageW(window, WM_MOUSEMOVE, 0, PointerPosition(9.0f, 9.0f, scale));
            ScriptEquals(*view,
                         L"return document.getElementById('label').textContent+'|'+window.lastTarget+'|'+window.lastRelated+'|'+window.overCount+'|'+window.gridEnterCount;",
                         L"1 x 1|b11||1|1",
                         L"pointerover bubbles from the first grid cell and pointerenter enters the grid once");

            SendMessageW(window, WM_MOUSEMOVE, 0, PointerPosition(55.0f, 32.0f, scale));
            ScriptEquals(*view,
                         L"return document.getElementById('label').textContent+'|'+window.lastTarget+'|'+window.lastRelated+'|'+window.overCount+'|'+window.gridEnterCount;",
                         L"3 x 2|b23|b11|2|1",
                         L"moving between cells sends a new pointerover with the previous relatedTarget");
            ScriptEquals(*view,
                         L"return document.querySelectorAll('#grid button.is-selected').length+'|'+window.moveCount;",
                         L"6|2",
                         L"the delegated pointer handler can expand the whole grid selection");

            ScriptEquals(*view,
                         L"return (Math.abs(window.lastClientX-55)<0.01)+'|'+(Math.abs(window.lastClientY-32)<0.01)+'|'+(Math.abs(window.lastMovementX-46)<0.01)+'|'+(Math.abs(window.lastMovementY-23)<0.01)+'|'+(Math.abs(window.devicePixelRatio-(window.innerWidth/240))<0.01);",
                         L"true|true|true|true|true",
                         L"pointer coordinates stay in CSS pixels at the active DPI");

            SendMessageW(window, WM_MOUSELEAVE, 0, 0);
        }
        view.reset();
        DestroyWindow(host);
    }
    SetThreadDpiAwarenessContext(previousContext);
    return dpi;
}

} // namespace

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CheckCommonEngineScale(1.0f);
    CheckCommonEngineScale(1.5f);
    const UINT dpi100 = CheckPointerGridAtDpi(DPI_AWARENESS_CONTEXT_UNAWARE);
    const UINT monitorDpi = CheckPointerGridAtDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(dpi100 == USER_DEFAULT_SCREEN_DPI,
          L"the DPI-unaware pointer path uses 100 percent coordinates");
    Check(monitorDpi >= USER_DEFAULT_SCREEN_DPI,
          L"the per-monitor pointer path uses the monitor DPI coordinates");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Pointer-event grid regression tests passed at 100%, 150%, and monitor DPI ("
               << monitorDpi << L" DPI)\n";
    return 0;
}
