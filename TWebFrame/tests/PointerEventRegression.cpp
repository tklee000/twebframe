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
                    #capture { position: absolute; left: 80px; top: 20px; width: 20px; height: 20px; }
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
                <button id="capture"></button>
                <dialog id="form-dialog"><form id="dialog-form" method="dialog"><input id="dialog-input"><button id="dialog-submit" type="submit" value="accepted">Submit</button></form></dialog>
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
                    window.captureMoves = 0;
                    window.captureUps = 0;
                    window.captureMouseUps = 0;
                    window.captureOnUp = false;
                    window.captureClientX = 0;
                    window.captureClientY = 0;
                    window.captureKeyUps = 0;
                    window.captureCancels = 0;
                    window.contextMenus = 0;
                    window.contextTarget = "";
                    window.contextClientX = 0;
                    window.dialogSubmits = 0;
                    window.dialogCancels = 0;
                    window.dialogCloses = 0;
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
                    const capture = document.getElementById("capture");
                    capture.addEventListener("pointerdown", (event) => capture.setPointerCapture(event.pointerId));
                    capture.addEventListener("pointermove", (event) => {
                        window.captureMoves += 1;
                        window.captureClientX = event.clientX;
                        window.captureClientY = event.clientY;
                    });
                    capture.addEventListener("pointerup", (event) => {
                        window.captureUps += 1;
                        window.captureOnUp = capture.hasPointerCapture(event.pointerId);
                        capture.releasePointerCapture(event.pointerId);
                    });
                    capture.addEventListener("mouseup", () => { window.captureMouseUps += 1; });
                    capture.addEventListener("keyup", () => { window.captureKeyUps += 1; });
                    capture.addEventListener("pointercancel", (event) => {
                        window.captureCancels += 1;
                        if (capture.hasPointerCapture(event.pointerId)) capture.releasePointerCapture(event.pointerId);
                    });
                    grid.addEventListener("contextmenu", (event) => {
                        event.preventDefault();
                        window.contextMenus += 1;
                        window.contextTarget = event.target.id;
                        window.contextClientX = event.clientX;
                    });
                    const formDialog = document.getElementById("form-dialog");
                    document.getElementById("dialog-form").addEventListener("submit", () => { window.dialogSubmits += 1; });
                    formDialog.addEventListener("cancel", () => { window.dialogCancels += 1; });
                    formDialog.addEventListener("close", () => { window.dialogCloses += 1; });
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

            const float firstX=std::round(9.0f*scale)/scale;
            const float firstY=std::round(9.0f*scale)/scale;
            const float secondX=std::round(55.0f*scale)/scale;
            const float secondY=std::round(32.0f*scale)/scale;
            const std::wstring coordinateScript=
                L"return (Math.abs(window.lastClientX-"+std::to_wstring(secondX)+L")<0.01)+'|'+"+
                L"(Math.abs(window.lastClientY-"+std::to_wstring(secondY)+L")<0.01)+'|'+"+
                L"(Math.abs(window.lastMovementX-"+std::to_wstring(secondX-firstX)+L")<0.01)+'|'+"+
                L"(Math.abs(window.lastMovementY-"+std::to_wstring(secondY-firstY)+L")<0.01)+'|'+"+
                L"(Math.abs(window.devicePixelRatio-"+std::to_wstring(scale)+L")<0.01);";
            ScriptEquals(*view,coordinateScript.c_str(),
                         L"true|true|true|true|true",
                         L"pointer coordinates stay in CSS pixels at the active DPI");

            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, PointerPosition(90.0f, 30.0f, scale));
            Check(GetCapture() == window, L"setPointerCapture acquires native capture at the active DPI");
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, PointerPosition(180.0f, 90.0f, scale));
            SendMessageW(window, WM_LBUTTONUP, 0, PointerPosition(180.0f, 90.0f, scale));
            ScriptEquals(*view,
                         L"return window.captureMoves+'|'+window.captureUps+'|'+window.captureMouseUps+'|'+window.captureOnUp+'|'+document.getElementById('capture').hasPointerCapture(1)+'|'+(Math.abs(window.captureClientX-180)<0.01)+'|'+(Math.abs(window.captureClientY-90)<0.01);",
                         L"1|1|1|true|false|true|true",
                         L"captured pointermove and pointerup remain in CSS-pixel coordinates outside the element");
            Check(GetCapture() != window, L"pointerup releases native pointer capture");
            SendMessageW(window, WM_KEYUP, L'A', 0);
            ScriptEquals(*view,L"return window.captureKeyUps;",L"1",
                         L"native key release dispatches the common DOM keyup event");
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, PointerPosition(90.0f, 30.0f, scale));
            ReleaseCapture();
            ScriptEquals(*view,L"return window.captureCancels+'|'+document.getElementById('capture').hasPointerCapture(1);",L"1|false",
                         L"unexpected native capture loss dispatches pointercancel and clears DOM capture");

            POINT contextPoint{static_cast<LONG>(std::lround(9.0f*scale)),static_cast<LONG>(std::lround(9.0f*scale))};
            ClientToScreen(window,&contextPoint);
            SendMessageW(window,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(window),MAKELPARAM(contextPoint.x,contextPoint.y));
            const auto expectedContextX=std::round(9.0f*scale)/scale;
            const auto contextScript=L"return window.contextMenus+'|'+window.contextTarget+'|'+(Math.abs(window.contextClientX-"+
                std::to_wstring(expectedContextX)+L")<0.01);";
            ScriptEquals(*view,contextScript.c_str(),L"1|b11|true",
                         L"contextmenu targets the CSS-pixel hit at the active DPI");

            ScriptEquals(*view,L"const dialog=document.getElementById('form-dialog');dialog.showModal();document.getElementById('dialog-submit').click();return window.dialogSubmits+'|'+dialog.open+'|'+window.dialogCloses;",L"1|false|1",
                         L"programmatic submit-button activation submits and closes method-dialog forms");
            ScriptEquals(*view,L"const dialog=document.getElementById('form-dialog');dialog.showModal();document.getElementById('dialog-input').focus();return dialog.open;",L"true",
                         L"dialog can reopen and focus its text input");
            SendMessageW(window,WM_KEYDOWN,VK_RETURN,0);
            ScriptEquals(*view,L"return window.dialogSubmits+'|'+document.getElementById('form-dialog').open+'|'+window.dialogCloses;",L"2|false|2",
                         L"Enter in a text input submits its nearest form");
            ScriptEquals(*view,L"const dialog=document.getElementById('form-dialog');dialog.showModal();document.getElementById('dialog-input').focus();return dialog.open;",L"true",
                         L"dialog reopens for Escape cancellation");
            SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);
            ScriptEquals(*view,L"return window.dialogCancels+'|'+document.getElementById('form-dialog').open+'|'+window.dialogCloses;",L"1|false|3",
                         L"Escape dispatches cancel and performs the dialog close default action");

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
