#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace TWebFrame::Internal;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

} // namespace

int wmain() {
    const wchar_t* html = LR"HTML(
        <style>
            * { box-sizing: border-box; margin: 0; padding: 0; }
            .tabs { display: flex; height: 40px; }
            .tab { width: 120px; height: 32px; background: transparent; transition: background 0.2s; }
            .tab.active { background: #cfe8fa; }
            .tab:hover:not(.active) { background: #eef5fb; }
            .panel { display: none; }
            .panel.active { display: block; }
        </style>
        <div class="tabs" id="tabs">
            <button id="first" class="tab active" onclick="switchPanel('panel-one')">First</button>
            <button id="second" class="tab" onclick="inlineContext = this === event.currentTarget; switchPanel('panel-two')">Second</button>
        </div>
        <button id="cancel" onclick="return false">Cancel default</button>
        <div id="panel-one" class="panel active">One</div>
        <div id="panel-two" class="panel">Two</div>
        <script>
            let inlineContext = false;
            let eventOrder = '';
            let removableCalls = 0;
            let passivePrevented = true;
            function switchPanel(panelId) {
                document.querySelectorAll('.panel').forEach(panel => panel.classList.remove('active'));
                document.querySelectorAll('.tab').forEach(tab => tab.classList.remove('active'));
                document.getElementById(panelId).classList.add('active');
                event.currentTarget.classList.add('active');
            }
            function mark(value) { eventOrder += value; }
            function removable() { removableCalls += 1; }
            window.addEventListener('probe', () => mark('W'), true);
            document.addEventListener('probe', () => mark('D'), { capture: true });
            document.getElementById('tabs').addEventListener('probe', () => mark('O'), true);
            document.getElementById('second').addEventListener('probe', () => mark('T'), { capture: true });
            document.getElementById('second').addEventListener('probe', () => mark('t'));
            document.getElementById('second').addEventListener('probe', () => mark('1'), { once: true });
            document.getElementById('tabs').addEventListener('probe', () => mark('o'));
            document.addEventListener('probe', () => mark('d'));
            window.addEventListener('probe', () => mark('w'));
            document.getElementById('second').addEventListener('remove-probe', removable, true);
            document.getElementById('second').removeEventListener('remove-probe', removable, false);
            document.getElementById('second').addEventListener('passive-probe', event => {
                event.preventDefault(); passivePrevented = event.defaultPrevented;
            }, { passive: true });
        </script>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());

    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());

    JavaScriptRuntime javascript(document);
    Check(javascript.Load(document.ScriptText(), &error), error.c_str());

    LayoutEngine layout(document, styles);
    layout.Layout(600.0f, 300.0f, 1.0f);

    const auto second = document.GetElementById(L"second");
    javascript.DispatchNodeEvent(second, L"click");
    second->hovered = true;

    std::wstring inlineContext;
    Check(javascript.Execute(L"return inlineContext;", &inlineContext, &error) &&
              inlineContext == L"true",
          L"an inline handler receives the current element as `this`");
    Check(javascript.DispatchNodeEvent(document.GetElementById(L"cancel"), L"click"),
          L"returning false from an inline handler prevents the default action");

    javascript.DispatchNodeEvent(second, L"probe");
    javascript.DispatchNodeEvent(second, L"probe");
    std::wstring eventOrder;
    Check(javascript.Execute(L"return eventOrder;", &eventOrder, &error) &&
              eventOrder == L"WDOTt1odwWDOTtodw",
          L"capture, target, bubble and once listeners follow shared DOM dispatch order");

    javascript.DispatchNodeEvent(second, L"remove-probe");
    Check(javascript.Execute(L"document.getElementById('second').removeEventListener('remove-probe', removable, true); return removableCalls;", &inlineContext, &error) &&
              inlineContext == L"1",
          L"removeEventListener matches both callback identity and capture flag");
    javascript.DispatchNodeEvent(second, L"remove-probe");
    Check(javascript.Execute(L"return removableCalls;", &inlineContext, &error) &&
              inlineContext == L"1",
          L"a removed listener is not invoked by a later dispatch");

    Check(!javascript.DispatchNodeEvent(second, L"passive-probe"),
          L"a passive listener cannot cancel the default action");
    Check(javascript.Execute(L"return passivePrevented;", &inlineContext, &error) &&
              inlineContext == L"false",
          L"preventDefault remains ineffective while a passive listener runs");

    Check(!document.GetElementById(L"first")->HasClass(L"active") &&
              second->HasClass(L"active"),
          L"an inline click handler retains its target while updating classes");
    Check(!document.GetElementById(L"panel-one")->HasClass(L"active") &&
              document.GetElementById(L"panel-two")->HasClass(L"active"),
          L"the same inline handler can update associated content through common DOM APIs");

    for (const float scale : {1.0f, 1.5f}) {
        layout.Layout(600.0f, 300.0f, scale);
        const auto* firstBox = layout.BoxFor(document.GetElementById(L"first"));
        const auto* secondBox = layout.BoxFor(second);
        Check(firstBox && secondBox, L"tab controls remain in the layout tree");
        Check(firstBox && firstBox->style.Get(L"background") == L"transparent",
              L"the inactive tab uses its base background");
        Check(secondBox && secondBox->style.Get(L"background") == L"#cfe8fa",
              L"the active class updates the computed background");
        if (secondBox) {
            const float physicalX = std::round((secondBox->rect.x + secondBox->rect.width / 2.0f) * scale);
            const float physicalY = std::round((secondBox->rect.y + secondBox->rect.height / 2.0f) * scale);
            const auto hit = layout.HitTest(physicalX / scale, physicalY / scale);
            Check(hit == second, L"CSS-pixel hit testing is stable across device scales");
        }
    }

    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Inline event regression tests passed\n";
    return 0;
}
