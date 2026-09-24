#include "CSS.h"
#include "DOM.h"
#include "Layout.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;

namespace {
int failures = 0;
void Check(bool ok, const wchar_t* message) {
    if (!ok) { ++failures; std::wcerr << L"FAIL: " << message << L'\n'; }
}

// A real raster target with an explicit DPI, independent of the test machine's
// monitor settings. All layout and pointer coordinates remain CSS pixels.
struct Raster {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    UINT width, height;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteFactory> text;
    Raster(float scale) : width(static_cast<UINT>(800 * scale)),
                          height(static_cast<UINT>(400 * scale)) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap) previous = SelectObject(dc, bitmap);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        Check(bitmap && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
            SUCCEEDED(factory->CreateDCRenderTarget(&props, &target)) &&
            SUCCEEDED(target->BindDC(dc, &bounds)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(text.GetAddressOf()))), L"DPI raster target initializes");
        if (target) target->SetDpi(96 * scale, 96 * scale);
    }
    ~Raster() {
        target.Reset();
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void Paint(LayoutEngine& layout, const LayoutRect* dirty = nullptr) {
        target->BeginDraw();
        if (dirty) target->PushAxisAlignedClip(D2D1::RectF(dirty->x, dirty->y,
            dirty->x + dirty->width, dirty->y + dirty->height), D2D1_ANTIALIAS_MODE_ALIASED);
        target->Clear(D2D1::ColorF(D2D1::ColorF::White));
        layout.Paint(target.Get(), text.Get(), dirty);
        if (dirty) target->PopAxisAlignedClip();
        Check(SUCCEEDED(target->EndDraw()), L"scroll frame renders");
    }
    std::vector<std::uint32_t> Snapshot() {
        GdiFlush();
        const auto begin = static_cast<const std::uint32_t*>(pixels);
        return {begin, begin + static_cast<size_t>(width) * height};
    }
};

std::wstring Fixture(size_t rows, bool positioned) {
    std::wstring html = LR"HTML(<style>
        *{box-sizing:border-box;margin:0;padding:0}
        body{font:12px Segoe UI;background:white}
        .viewport{width:640px;height:300px;overflow:auto}
        .heading{height:24px;position:sticky;top:0;z-index:4;background:#abcdef}
        .row{display:grid;grid-template-columns:70px 70px 1fr;width:900px;height:24px}
        .cell{white-space:pre;overflow:hidden;border-bottom:1px solid #ddd}
        .row:nth-child(even){background:#eef2fa}
        .positioned .cell{position:relative;z-index:2}
        .aside{height:80px;background:#ddbb99}
        </style><div class='viewport )HTML";
    html += positioned ? L"positioned" : L"plain";
    html += L"'><div class='heading'>Sticky heading</div>";
    for (size_t i = 0; i < rows; ++i)
        html += L"<div class='row'><span class='cell'>" + std::to_wstring(i) +
            L"</span><span class='cell'>" + std::to_wstring(i + 1) +
            L"</span><span class='cell'>A long grid row with selectable text</span></div>";
    html += L"</div><div class='aside'>Unrelated sibling remains unchanged</div>";
    return html;
}

void Run(float scale, bool positioned, size_t rows, bool benchmark) {
    Document document;
    StyleSheet styles;
    Check(document.Parse(Fixture(rows, positioned)), L"generic scrolling grid parses");
    Check(styles.Parse(document.StyleText()), L"generic scrolling CSS parses");
    LayoutEngine layout(document, styles);
    layout.Layout(800, 400, scale);
    Raster raster(scale);
    if (!raster.target) return;
    const auto viewport = document.QuerySelector(L".viewport");
    const auto heading = document.QuerySelector(L".heading");
    const auto firstCell = document.QuerySelector(L".cell");
    const auto* viewportBox = layout.BoxFor(viewport);
    Check(viewportBox && viewportBox->scrollWidth >= 900, L"horizontal overflow is retained");
    raster.Paint(layout);
    const auto initial = raster.Snapshot();
    const float firstY = layout.BoxFor(firstCell)->rect.y;
    std::vector<double> inputTimes, paintTimes;
    const size_t frames = benchmark ? 80 : 8;
    const LayoutRect dirty = viewportBox->rect;
    for (size_t i = 0; i < frames; ++i) {
        const auto start = std::chrono::steady_clock::now();
        std::shared_ptr<Node> scrolled;
        Check(layout.ScrollAt(std::round(100 * scale) / scale,
            std::round(150 * scale) / scale, i % 2 ? 120.0f : -120.0f, &scrolled) &&
            scrolled == viewport, L"wheel follows the visible row's scroll chain");
        const auto inputEnd = std::chrono::steady_clock::now();
        raster.Paint(layout, &dirty);
        const auto paintEnd = std::chrono::steady_clock::now();
        if (i >= 4) {
            inputTimes.push_back(std::chrono::duration<double, std::milli>(inputEnd - start).count());
            paintTimes.push_back(std::chrono::duration<double, std::milli>(paintEnd - inputEnd).count());
        }
        Check(std::abs(layout.BoxFor(heading)->rect.y - viewportBox->content.y) < .01f,
            L"sticky header stays pinned across repeated wheel scrolling");
        Check(layout.HitTest(20, 10) == heading, L"sticky header preserves stacking and hit testing");
    }
    Check(std::abs(layout.BoxFor(firstCell)->rect.y - firstY) < .01f,
        L"back-and-forth scrolling does not accumulate geometry drift");
    Check(initial == raster.Snapshot(), L"dirty scroll repaint restores identical pixels and preserves siblings");

    viewport->scrollTop = 137.5f;
    viewport->scrollLeft = 45.5f;
    Check(layout.SyncScroll(viewport), L"fractional DOM scroll synchronizes both axes");
    raster.Paint(layout);
    const auto incremental = raster.Snapshot();
    const auto hit = layout.HitTest(120, 150);
    layout.Layout(800, 400, scale);
    raster.Paint(layout);
    Check(incremental == raster.Snapshot(), L"incremental scrolling matches a fresh layout at fractional offsets");
    Check(hit == layout.HitTest(120, 150), L"incremental and fresh layout agree on the pointer target");
    layout.Relayout(800, 400, scale == 1 ? 1.5f : 1.0f);
    viewport->scrollTop += 24;
    Check(layout.SyncScroll(viewport), L"scroll metadata is rebuilt after a DPI change");
    Check(std::abs(layout.BoxFor(heading)->rect.y - layout.BoxFor(viewport)->content.y) < .01f,
        L"sticky geometry remains in CSS pixels after a DPI change");
    if (benchmark) {
        const auto p50 = [](std::vector<double> values) {
            std::sort(values.begin(), values.end()); return values[values.size() / 2];
        };
        std::wcout << (positioned ? L"positioned_grid" : L"plain_grid") << L',' << rows << L','
            << scale << L',' << p50(inputTimes) << L',' << p50(paintTimes) << L'\n';
    }
    layout.DiscardDeviceResources();
}

void CheckNestedOverflow(float scale) {
    Document document;
    StyleSheet styles;
    Check(document.Parse(LR"HTML(<style>
        *{box-sizing:border-box;margin:0;padding:0}
        .outer{width:360px;height:180px;overflow:auto}
        .lead{height:36px}
        .inner{width:300px;height:72px;overflow:auto}
        .content{width:500px;height:400px;background:#ccbbee}
        .tail{height:240px;background:#bbeedd}
        .fixed{position:fixed;left:420px;top:20px;width:60px;height:30px;z-index:4;background:#eeaa88}
        .context{position:relative;z-index:1;width:10px;height:10px}
        .popup{position:absolute;left:30px;top:0;width:80px;height:30px;z-index:2;background:#99ccff}
        .clipper{position:relative;width:20px;height:20px;overflow:hidden}
        .clipped{position:absolute;left:30px;top:0;width:80px;height:30px;z-index:5}
        </style><div class='outer'><div class='lead'></div>
        <div class='inner'><div class='content'></div></div>
        <div class='tail'></div><div class='fixed'></div></div>
        <div class='context'><div class='popup'></div></div>
        <div class='clipper'><div class='clipped'></div></div>)HTML"),
        L"nested overflow fixture parses");
    Check(styles.Parse(document.StyleText()), L"nested overflow styles parse");
    LayoutEngine layout(document, styles);
    layout.Layout(800, 400, scale);
    Raster raster(scale);
    if (!raster.target) return;
    const auto outer = document.QuerySelector(L".outer");
    const auto inner = document.QuerySelector(L".inner");
    const auto fixed = document.QuerySelector(L".fixed");
    const auto popup = document.QuerySelector(L".popup");
    const auto clipped = document.QuerySelector(L".clipped");
    const auto fixedRect = layout.BoxFor(fixed)->rect;
    const auto popupRect = layout.BoxFor(popup)->rect;
    Check(layout.HitTest(popupRect.x + 5, popupRect.y + 5) == popup,
        L"a stacking context's overflowing descendant remains hit-testable outside its parent");
    const auto clippedRect = layout.BoxFor(clipped)->rect;
    Check(layout.HitTest(clippedRect.x + 5, clippedRect.y + 5) != clipped,
        L"ancestor overflow still clips a deferred stacking context");
    inner->scrollTop = 45.5f;
    inner->scrollLeft = 18.5f;
    Check(layout.SyncScroll(inner), L"nested container scrolls on both axes");
    outer->scrollTop = 25.5f;
    Check(layout.SyncScroll(outer), L"outer scrolling preserves the inner scroll offset");
    inner->scrollTop += 24;
    Check(layout.SyncScroll(inner), L"nested scrolling works after ancestor translation");
    Check(std::abs(layout.BoxFor(fixed)->rect.y - fixedRect.y) < .01f &&
        std::abs(layout.BoxFor(fixed)->rect.x - fixedRect.x) < .01f,
        L"fixed children are excluded from scroll translation");
    std::vector<std::shared_ptr<Node>> targets;
    for (float y = 8; y < 240; y += 16)
        for (float x = 8; x < 500; x += 16) targets.push_back(layout.HitTest(x, y));
    raster.Paint(layout);
    const auto pixels = raster.Snapshot();
    Check(std::abs(layout.BoxFor(outer)->scrollWidth - 360) < .01f &&
        std::abs(layout.BoxFor(outer)->scrollHeight - 348) < .01f,
        L"nested clipped content and fixed children do not inflate outer scroll ranges");
    layout.Layout(800, 400, scale);
    raster.Paint(layout);
    Check(pixels == raster.Snapshot(), L"nested incremental scrolling matches a full raster rebuild");
    size_t index = 0;
    for (float y = 8; y < 240; y += 16)
        for (float x = 8; x < 500; x += 16)
            Check(targets[index++] == layout.HitTest(x, y),
                L"nested cached bounds agree with rebuilt pointer geometry");
    // Replace descendants and resize to catch stale cache pointers as well as
    // ranges that become scrollable only after a new layout.
    document.SetInnerHtml(inner, L"<div style='width:600px;height:500px;background:#aabbcc'></div>");
    layout.Layout(800, 400, scale);
    layout.Relayout(720, 360, scale);
    inner->scrollTop += 25;
    Check(layout.SyncScroll(inner) && layout.BoxFor(inner)->scrollHeight >= 500,
        L"DOM replacement and resize rebuild the scroll traversal cache");
    raster.Paint(layout);
    layout.DiscardDeviceResources();
}
}

int wmain(int argc, wchar_t** argv) {
    const bool benchmark = argc > 1 && std::wstring(argv[1]) == L"--benchmark";
    std::wcout << std::fixed << std::setprecision(3);
    if (benchmark) std::wcout << L"scenario,rows,dpi_scale,input_p50_ms,paint_p50_ms\n";
    for (float scale : {1.0f, 1.5f}) {
        for (bool positioned : {false, true}) Run(scale, positioned, benchmark ? 3000 : 80, benchmark);
        CheckNestedOverflow(scale);
    }
    if (failures) { std::wcerr << failures << L" checks failed\n"; return 1; }
    std::wcout << L"Scroll rendering regression passed at 100% and 150% DPI\n";
    return 0;
}
