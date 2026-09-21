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
        <div class="tabs">
            <button id="first" class="tab active" onclick="switchPanel('panel-one')">First</button>
            <button id="second" class="tab" onclick="inlineContext = this === event.currentTarget; switchPanel('panel-two')">Second</button>
        </div>
        <button id="cancel" onclick="return false">Cancel default</button>
        <div id="panel-one" class="panel active">One</div>
        <div id="panel-two" class="panel">Two</div>
        <script>
            let inlineContext = false;
            function switchPanel(panelId) {
                document.querySelectorAll('.panel').forEach(panel => panel.classList.remove('active'));
                document.querySelectorAll('.tab').forEach(tab => tab.classList.remove('active'));
                document.getElementById(panelId).classList.add('active');
                event.currentTarget.classList.add('active');
            }
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
