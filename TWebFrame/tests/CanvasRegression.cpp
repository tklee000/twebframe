#include "Canvas.h"
#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <d2d1.h>
#include <dwrite.h>
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <iostream>
#include <string>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

bool Near(float left, float right) { return std::abs(left - right) < 0.01f; }

void CheckRaster(LayoutEngine& layout, float scale) {
    constexpr float viewportWidth = 700.0f;
    constexpr float viewportHeight = 460.0f;
    const auto pixelWidth = static_cast<UINT>(std::lround(viewportWidth * scale));
    const auto pixelHeight = static_cast<UINT>(std::lround(viewportHeight * scale));
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> writeFactory;
    const bool resources =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory))) &&
        SUCCEEDED(wicFactory->CreateBitmap(pixelWidth, pixelHeight,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2dFactory.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(d2dFactory->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                USER_DEFAULT_SCREEN_DPI * scale, USER_DEFAULT_SCREEN_DPI * scale),
            &target)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())));
    Check(resources, L"canvas raster resources are created");
    if (!resources) return;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    layout.Paint(target.Get(), writeFactory.Get());
    Check(SUCCEEDED(target->EndDraw()), L"canvas commands paint successfully");

    WICRect lockRect{0, 0, static_cast<INT>(pixelWidth), static_cast<INT>(pixelHeight)};
    ComPtr<IWICBitmapLock> lock;
    Check(SUCCEEDED(bitmap->Lock(&lockRect, WICBitmapLockRead, &lock)),
          L"canvas pixels are readable");
    if (!lock) return;
    UINT stride = 0, bufferSize = 0;
    BYTE* bytes = nullptr;
    Check(SUCCEEDED(lock->GetStride(&stride)) &&
              SUCCEEDED(lock->GetDataPointer(&bufferSize, &bytes)) && bytes,
          L"canvas pixel buffer is available");
    if (!bytes) return;
    const auto pixel = [&](float cssX, float cssY) {
        const auto x = std::min(pixelWidth - 1,
            static_cast<UINT>(std::max(0.0f, std::round(cssX * scale))));
        const auto y = std::min(pixelHeight - 1,
            static_cast<UINT>(std::max(0.0f, std::round(cssY * scale))));
        return bytes + y * stride + x * 4;
    };

    const auto* background = pixel(10.0f, 10.0f);
    Check(background[3] > 240 && background[0] > 235 &&
              background[1] > 235 && background[2] > 235,
          L"the intrinsic canvas bitmap is composited into its CSS box");
    const auto* point = pixel(470.0f * 640.0f / 960.0f,
                              137.0f * 420.0f / 630.0f);
    Check(point[3] > 240 && point[0] > 100 && point[0] < 220 &&
              point[1] > 100 && point[1] < 220 && point[2] > 100 && point[2] < 220,
          L"canvas paths retain their intrinsic coordinates when CSS-scaled");
    bool foundStroke = false;
    const int strokeCenter = static_cast<int>(std::lround(550.0f * 420.0f / 630.0f * scale));
    const UINT strokeX = static_cast<UINT>(std::lround(320.0f * scale));
    for (int y = std::max(0, strokeCenter - 4);
         y <= std::min<int>(pixelHeight - 1, strokeCenter + 4); ++y) {
        const auto* sample = bytes + static_cast<UINT>(y) * stride + strokeX * 4;
        foundStroke = foundStroke ||
            (sample[3] > 240 && sample[0] < 80 && sample[1] < 80 && sample[2] < 80);
    }
    Check(foundStroke, L"canvas strokes scale consistently at 100% and 150% DPI");
    Check(pixel(680.0f, 10.0f)[3] == 0,
          L"painting remains clipped to the canvas CSS content box");
}

} // namespace

int wmain() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const wchar_t* html = LR"HTML(
        <style>
            html, body { margin: 0; }
            canvas { width: 640px; height: 420px; }
        </style>
        <canvas id="chart" width="960" height="630"></canvas>
        <script>
            var canvas = document.getElementById('chart');
            var context = canvas.getContext('2d');
            context.setTransform(1, 0, 0, 1, 0, 0);
            context.clearRect(0, 0, canvas.width, canvas.height);
            var gradient = context.createLinearGradient(0, 0, 0, canvas.height);
            gradient.addColorStop(0, '#fff');
            gradient.addColorStop(1, '#f7f7f7');
            context.fillStyle = gradient;
            context.fillRect(0, 0, canvas.width, canvas.height);
            context.beginPath();
            context.moveTo(90, 550);
            context.lineTo(930, 550);
            context.lineWidth = 3;
            context.strokeStyle = '#222';
            context.stroke();
            context.beginPath();
            context.arc(470, 137, 10, 0, Math.PI * 2, false);
            context.fillStyle = '#9e9e9e';
            context.fill();
            context.save();
            context.translate(24, 295);
            context.rotate(-Math.PI / 2);
            context.font = "20px Arial, '맑은 고딕', sans-serif";
            context.textAlign = 'center';
            context.textBaseline = 'top';
            context.fillStyle = '#000';
            context.fillText('빈도', 0, 0);
            context.restore();
            var runtimeProbe = Infinity > 1 && Math.round(Math.cos(0)) === 1 &&
                               Math.round(Math.sin(Math.PI / 2)) === 1 &&
                               Math.round(Math.atan2(1, 0) * 2) === Math.round(Math.PI);
        </script>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());

    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());

    JavaScriptRuntime javascript(document);
    javascript.SetDevicePixelRatio(1.5);
    int paintMutations = 0;
    javascript.SetMutationSink([&](const JavaScriptRuntime::Mutation& mutation) {
        if (mutation.kind == JavaScriptRuntime::MutationKind::Paint) ++paintMutations;
    });
    Check(javascript.Load(document.ScriptText(), &error), error.c_str());

    const auto canvas = document.GetElementById(L"chart");
    Check(canvas && canvas->canvas, L"getContext creates a shared canvas surface");
    Check(canvas && canvas->canvas && canvas->canvas->width == 960 &&
              canvas->canvas->height == 630,
          L"canvas backing-store dimensions come from the width and height attributes");
    Check(canvas && canvas->canvas && canvas->canvas->commands.size() == 4,
          L"fill, stroke, arc and text drawing commands are retained in order");
    Check(paintMutations >= 1, L"drawing invalidates the canvas paint region");

    std::wstring probe;
    Check(javascript.Execute(L"return runtimeProbe && window.devicePixelRatio === 1.5;",
                             &probe, &error) && probe == L"true",
          L"canvas scripts receive standard numeric globals and the monitor pixel ratio");

    Document resetDocument;
    Check(resetDocument.Parse(L"<canvas></canvas>", &error), error.c_str());
    JavaScriptRuntime resetRuntime(resetDocument);
    Check(resetRuntime.Execute(
              L"const canvas=document.querySelector('canvas');"
              L"const context=canvas.getContext('2d');"
              L"context.lineWidth=7;context.fillRect(0,0,10,10);"
              L"canvas.width=canvas.width;const propertyReset=context.lineWidth===1;"
              L"context.lineWidth=5;context.fillRect(0,0,10,10);"
              L"canvas.setAttribute('height','75');const attributeReset=context.lineWidth===1;"
              L"context.lineWidth=3;context.fillRect(0,0,10,10);"
              L"canvas.removeAttribute('height');"
              L"return propertyReset&&attributeReset&&context.lineWidth===1&&canvas.height===150;",
              &probe, &error) && probe == L"true",
          L"canvas dimension mutations reset drawing state and the backing store");
    const auto resetCanvas = resetDocument.QuerySelector(L"canvas");
    Check(resetCanvas && resetCanvas->canvas && resetCanvas->canvas->commands.empty(),
          L"resetting a canvas dimension clears retained pixels even when the size is unchanged");

    LayoutEngine layout(document, styles);
    for (const float scale : {1.0f, 1.5f}) {
        layout.Layout(700.0f, 460.0f, scale);
        const auto* box = layout.BoxFor(canvas);
        Check(box && Near(box->content.width, 640.0f) && Near(box->content.height, 420.0f),
              L"CSS canvas geometry stays in DIPs at 100% and 150% DPI");
        CheckRaster(layout, scale);
    }

    if (SUCCEEDED(initialized)) CoUninitialize();

    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Canvas regression tests passed at 100% and 150% scaling\n";
    return 0;
}
