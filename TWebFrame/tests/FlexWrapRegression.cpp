#include "CSS.h"
#include "DOM.h"
#include "Layout.h"

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
            .status-bar { width: 100%; height: 54px; padding: 0 18px;
                          display: flex; align-items: center; gap: 22px; }
            .connection { width: 300px; height: 20px; flex-shrink: 0; }
            .bit { width: 150px; height: 20px; flex-shrink: 0; }
            .storage { min-width: 280px; flex-grow: 1; display: flex;
                       align-items: center; gap: 10px; height: 20px; }
            .storage-label, .available-label { white-space: nowrap; }
            .track { width: 250px; height: 12px; flex-shrink: 0; }
            @media (max-width: 1024px) {
                .status-bar { height: auto; min-height: 54px; padding: 10px 18px;
                              flex-wrap: wrap; }
            }
        </style>
        <div class="status-bar" id="header">
            <div class="connection" id="connection">GUI LINK</div>
            <div class="bit" id="bit">BIT</div>
            <div class="storage" id="storage">
                <span class="storage-label">Storage capacity</span>
                <span class="available-label">Available time</span>
                <div class="track"></div>
            </div>
        </div>
    )HTML";

    std::wstring error;
    Document narrowDocument;
    Check(narrowDocument.Parse(html, &error), error.c_str());
    StyleSheet narrowStyles;
    Check(narrowStyles.Parse(narrowDocument.StyleText(), &error), error.c_str());
    LayoutEngine narrow(narrowDocument, narrowStyles);
    narrow.Layout(920.0f, 260.0f, scale);

    const auto* header = narrow.BoxFor(narrowDocument.GetElementById(L"header"));
    const auto* connection = narrow.BoxFor(narrowDocument.GetElementById(L"connection"));
    const auto* storage = narrow.BoxFor(narrowDocument.GetElementById(L"storage"));
    Check(header && connection && storage, L"the responsive flex fixture is laid out");
    if (header && connection && storage) {
        Check(storage->rect.y >= connection->rect.y + connection->rect.height + 20.0f,
              L"a max-content flex item moves to the next line before its contents wrap");
        Check(header->rect.height > 70.0f,
              L"an auto-height wrapping flex container includes every flex line");
        Check(storage->rect.width > 800.0f,
              L"a growing item fills the main axis of its new flex line");
    }

    Document wideDocument;
    Check(wideDocument.Parse(html, &error), error.c_str());
    StyleSheet wideStyles;
    Check(wideStyles.Parse(wideDocument.StyleText(), &error), error.c_str());
    LayoutEngine wide(wideDocument, wideStyles);
    wide.Layout(1280.0f, 260.0f, scale);
    const auto* wideConnection = wide.BoxFor(wideDocument.GetElementById(L"connection"));
    const auto* wideStorage = wide.BoxFor(wideDocument.GetElementById(L"storage"));
    Check(wideConnection && wideStorage &&
              std::abs(wideConnection->rect.y - wideStorage->rect.y) < 0.01f,
          L"the same flex items remain on one line outside the media-query breakpoint");

    wide.Relayout(920.0f, 260.0f, scale);
    const auto* resizedConnection = wide.BoxFor(wideDocument.GetElementById(L"connection"));
    const auto* resizedStorage = wide.BoxFor(wideDocument.GetElementById(L"storage"));
    Check(resizedConnection && resizedStorage &&
              resizedStorage->rect.y >= resizedConnection->rect.y + resizedConnection->rect.height + 20.0f,
          L"crossing the media-query breakpoint during live resize creates a new flex line");

    wide.Relayout(1280.0f, 260.0f, scale);
    wideConnection = wide.BoxFor(wideDocument.GetElementById(L"connection"));
    wideStorage = wide.BoxFor(wideDocument.GetElementById(L"storage"));
    Check(wideConnection && wideStorage &&
              std::abs(wideConnection->rect.y - wideStorage->rect.y) < 0.01f,
          L"growing the viewport across the breakpoint restores the single flex line");
}

} // namespace

int wmain() {
    CheckScale(1.0f);
    CheckScale(1.5f);
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Flex wrap regression tests passed at 100% and 150% scaling\n";
    return 0;
}
