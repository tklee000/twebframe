#include "CSS.h"
#include "DOM.h"
#include "Layout.h"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace TWebFrame::Internal;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

void CheckScale(float scale) {
    const wchar_t* html = LR"HTML(
        <style>
            * { box-sizing: border-box; margin: 0; padding: 0; }
            #scroller { width: 360px; height: 96px; overflow-y: auto; }
            table { width: 100%; table-layout: fixed; border-collapse: collapse; }
            th { position: sticky; top: 0; height: 32px; background: #ffffff; }
            td { height: 32px; background: #d02020; }
        </style>
        <div id="scroller">
            <table>
                <thead><tr><th id="sticky-header">Header</th></tr></thead>
                <tbody>
                    <tr><td>Row 1</td></tr><tr><td>Row 2</td></tr>
                    <tr><td>Row 3</td></tr><tr><td>Row 4</td></tr>
                    <tr><td>Row 5</td></tr><tr><td>Row 6</td></tr>
                </tbody>
            </table>
        </div>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());
    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());
    LayoutEngine layout(document, styles);
    layout.Layout(420.0f, 180.0f, scale);

    const auto scroller = document.GetElementById(L"scroller");
    const auto header = document.GetElementById(L"sticky-header");
    const auto* initialScrollerBox = layout.BoxFor(scroller);
    Check(initialScrollerBox != nullptr, L"the overflow container is laid out");
    if (!initialScrollerBox) return;

    const float scrollX = initialScrollerBox->content.x + initialScrollerBox->content.width / 2.0f;
    const float scrollY = initialScrollerBox->content.y + initialScrollerBox->content.height * 0.75f;
    Check(layout.ScrollAt(scrollX, scrollY, -120.0f), L"the table overflow container scrolls");

    const auto* scrollerBox = layout.BoxFor(scroller);
    const auto* headerBox = layout.BoxFor(header);
    Check(scrollerBox && headerBox &&
              std::abs(headerBox->rect.y - scrollerBox->content.y) < 0.01f,
          L"the sticky header remains pinned to the scrollport start edge");
    if (!scrollerBox || !headerBox) return;

    const auto& contexts = layout.Root()->nonNegativeStackingContexts;
    Check(std::find(contexts.begin(), contexts.end(), headerBox) != contexts.end(),
          L"an auto-z-index sticky box participates in the non-negative stacking phase");

    const float cssX = headerBox->rect.x + headerBox->rect.width / 2.0f;
    const float cssY = headerBox->rect.y + headerBox->rect.height / 2.0f;
    const float physicalX = std::round(cssX * scale);
    const float physicalY = std::round(cssY * scale);
    Check(layout.HitTest(physicalX / scale, physicalY / scale) == header,
          L"a sticky header stays above scrolled body rows for painting and hit testing");
}

} // namespace

int wmain() {
    CheckScale(1.0f);
    CheckScale(1.5f);
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Sticky table regression tests passed\n";
    return 0;
}
