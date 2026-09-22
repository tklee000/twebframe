#include "DOM.h"
#include "JavaScript.h"
#include <TWebFrame/TWebFrame.h>

#include <crtdbg.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <iostream>
#include <string>

using namespace TWebFrame::Internal;

namespace {

struct HeapDelta {
    size_t blocks = 0;
    size_t bytes = 0;
};

struct GuiResourceDelta {
    long gdi = 0;
    long user = 0;
};

void ExerciseRuntime(const wchar_t* source, int reloads) {
    Document document;
    std::wstring error;
    JavaScriptRuntime runtime(document);
    for (int index = 0; index < reloads; ++index) {
        if (!runtime.Load(source, &error)) {
            std::wcerr << L"JavaScript setup failed: " << error << L'\n';
            std::abort();
        }
    }
}

HeapDelta MeasureRuntime(const wchar_t* source, int reloads) {
    _CrtMemState before{}, after{}, difference{};
    _CrtMemCheckpoint(&before);
    ExerciseRuntime(source, reloads);
    _CrtMemCheckpoint(&after);
    if (!_CrtMemDifference(&difference, &before, &after)) return {};
    return {difference.lCounts[_NORMAL_BLOCK], difference.lSizes[_NORMAL_BLOCK]};
}

void QueryAccessibility(IUIAutomation* automation, HWND window) {
    if (!automation || !window) return;
    IUIAutomationElement* element = nullptr;
    if (SUCCEEDED(automation->ElementFromHandle(window, &element)) && element)
        element->Release();
}

bool ExerciseViews(HWND host, IUIAutomation* automation, int count) {
    constexpr const wchar_t* html = LR"HTML(
        <style>
            html, body { margin: 0; font: 14px Arial; }
            canvas { width: 320px; height: 160px; }
        </style>
        <button style="width: 200px; height: 48px">focus teardown</button>
        <div>memory teardown</div><div id="mount"></div>
        <canvas id="chart" width="320" height="160"></canvas>
        <script>
            var root = {}; root.self = root;
            var currentRouteScript = null;
            function draw() {
                var context = document.getElementById('chart').getContext('2d');
                context.fillRect(10, 10, 100, 40);
            }
            function loadRoute() {
                if (currentRouteScript && currentRouteScript.parentNode)
                    currentRouteScript.parentNode.removeChild(currentRouteScript);
                var script = document.createElement('script');
                script.src = 'memory-route.js';
                script.dataset.dynamic = 'true';
                document.body.appendChild(script);
                currentRouteScript = script;
            }
            draw();
            for (var route = 0; route < 8; route += 1) loadRoute();
            location.hash = '#/complete';
        </script>
    )HTML";
    for (int index = 0; index < count; ++index) {
        RECT bounds{0, 0, 640, 360};
        auto view = TWebFrame::View::Create(host, bounds);
        if (view) view->SetResourceLoader([](const std::wstring& resource,
                                             std::wstring& content) {
            if (resource != L"memory-route.js") return false;
            content = LR"JS(
                var routeButton = document.createElement('button');
                routeButton.textContent = 'route';
                routeButton.addEventListener('click', function () { root.last = routeButton; });
                document.getElementById('mount').replaceChildren(routeButton);
                window.addEventListener('memory-probe', function () { root.last = routeButton; });
            )JS";
            return true;
        });
        if (!view || !view->NavigateToString(html)) return false;
        (void)view->DumpLayoutJson();
        RedrawWindow(view->Window(), nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        // Exercise the same common focus/accessibility provider path used by
        // login buttons and navigation links. UIAutomationCore may retain a
        // provider until the view explicitly disconnects it during teardown.
        const LPARAM point = MAKELPARAM(40, 24);
        SendMessageW(view->Window(), WM_MOUSEMOVE, 0, point);
        SendMessageW(view->Window(), WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(view->Window(), WM_LBUTTONUP, 0, point);
        // Repeated client queries exercise UIAutomationCore's provider cache
        // and TWebFrame's explicit disconnect path at view destruction. Using
        // the client API also consumes the WM_GETOBJECT marshaling result.
        for (int request = 0; automation && request < 8; ++request)
            QueryAccessibility(automation, view->Window());
    }
    return true;
}

HeapDelta MeasureViews(HWND host, IUIAutomation* automation, int count) {
    _CrtMemState before{}, after{}, difference{};
    _CrtMemCheckpoint(&before);
    if (!ExerciseViews(host, automation, count)) std::abort();
    _CrtMemCheckpoint(&after);
    if (!_CrtMemDifference(&difference, &before, &after)) return {};
#ifdef _DEBUG
    _CrtMemDumpStatistics(&difference);
    _CrtMemDumpAllObjectsSince(&before);
#endif
    return {difference.lCounts[_NORMAL_BLOCK], difference.lSizes[_NORMAL_BLOCK]};
}

HeapDelta MeasureRepeatedAccessibilityQueries(HWND host, IUIAutomation* automation) {
    RECT bounds{0, 0, 320, 180};
    auto view = TWebFrame::View::Create(host, bounds);
    if (!view || !view->NavigateToString(
            L"<style>html,body{margin:0}</style><button>accessibility</button>"))
        std::abort();
    QueryAccessibility(automation, view->Window());
    _CrtMemState before{}, after{}, difference{};
    _CrtMemCheckpoint(&before);
    for (int request = 0; request < 64; ++request)
        QueryAccessibility(automation, view->Window());
    _CrtMemCheckpoint(&after);
    if (!_CrtMemDifference(&difference, &before, &after)) return {};
    return {difference.lCounts[_NORMAL_BLOCK], difference.lSizes[_NORMAL_BLOCK]};
}

GuiResourceDelta MeasureGuiResources(HWND host, IUIAutomation* automation, int count) {
    const auto process = GetCurrentProcess();
    const DWORD beforeGdi = GetGuiResources(process, GR_GDIOBJECTS);
    const DWORD beforeUser = GetGuiResources(process, GR_USEROBJECTS);
    if (!ExerciseViews(host, automation, count)) std::abort();
    const DWORD afterGdi = GetGuiResources(process, GR_GDIOBJECTS);
    const DWORD afterUser = GetGuiResources(process, GR_USEROBJECTS);
    return {static_cast<long>(afterGdi) - static_cast<long>(beforeGdi),
            static_cast<long>(afterUser) - static_cast<long>(beforeUser)};
}

} // namespace

int wmain() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    constexpr const wchar_t* script = LR"JS(
        var root = {};
        root.self = root;
        var expression = /memory/i;
        expression.self = expression;
        function outer(value) {
            function inner() { return value; }
            return inner;
        }
        var retained = outer(7);
        for (var index = 0; index < 700; index += 1) {
            var detached = document.createElement('button');
            detached.addEventListener('click', function () { return 1; });
            detached = null;
        }
    )JS";

    // Prime CRT/STL one-time allocations so the measured cycle contains only
    // allocations owned by a fresh JavaScript runtime.
    ExerciseRuntime(script, 1);

    const auto oneRuntime = MeasureRuntime(script, 1);
    const auto repeatedLoads = MeasureRuntime(script, 5);
    OleInitialize(nullptr);
    IUIAutomation* automation = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&automation));
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP | WS_VISIBLE,
                                -10000, -10000, 640, 360, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!host || !ExerciseViews(host, automation, 1)) {
        std::wcerr << L"FAIL: could not create the TWebFrame view fixture\n";
        if (host) DestroyWindow(host);
        if (automation) automation->Release();
        OleUninitialize();
        return 1;
    }
    const auto oneView = MeasureViews(host, automation, 1);
    const auto repeatedViews = MeasureViews(host, automation, 5);
    const auto repeatedAccessibility = MeasureRepeatedAccessibilityQueries(host, automation);
    const auto guiResources = MeasureGuiResources(host, automation, 10);
    DestroyWindow(host);
    if (automation) automation->Release();
    OleUninitialize();
    std::wcout << L"runtime teardown delta: " << oneRuntime.blocks << L" blocks, "
               << oneRuntime.bytes << L" bytes\n";
    std::wcout << L"five-load teardown delta: " << repeatedLoads.blocks << L" blocks, "
               << repeatedLoads.bytes << L" bytes\n";
    std::wcout << L"view teardown delta: " << oneView.blocks << L" blocks, "
               << oneView.bytes << L" bytes\n";
    std::wcout << L"five-view teardown delta: " << repeatedViews.blocks << L" blocks, "
               << repeatedViews.bytes << L" bytes\n";
    std::wcout << L"repeated accessibility-query delta: " << repeatedAccessibility.blocks
               << L" blocks, " << repeatedAccessibility.bytes << L" bytes\n";
    std::wcout << L"ten-view GUI resource delta: " << guiResources.gdi << L" GDI, "
               << guiResources.user << L" USER objects\n";

    if (oneRuntime.blocks != 0 || oneRuntime.bytes != 0 ||
        repeatedLoads.blocks != 0 || repeatedLoads.bytes != 0 ||
        oneView.blocks != 0 || oneView.bytes != 0 ||
        repeatedViews.blocks != 0 || repeatedViews.bytes != 0 ||
        repeatedAccessibility.blocks != 0 || repeatedAccessibility.bytes != 0 ||
        guiResources.gdi != 0 || guiResources.user != 0) {
        std::wcerr << L"FAIL: TWebFrame retained heap allocations after teardown\n";
        return 1;
    }
    std::wcout << L"Memory leak regression tests passed\n";
    return 0;
}
