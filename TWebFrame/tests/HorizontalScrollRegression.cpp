#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
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
            #viewport { width: 200px; height: 80px; overflow-x: auto; overflow-y: hidden; }
            #content { width: 500px; height: 40px; }
        </style>
        <div id="viewport"><div id="content">wide content</div></div>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());
    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());
    LayoutEngine layout(document, styles);
    layout.Layout(600.0f, 300.0f, scale);

    const auto viewport=document.GetElementById(L"viewport");
    const auto content=document.GetElementById(L"content");
    const auto* viewportBox=layout.BoxFor(viewport);
    const auto* contentBox=layout.BoxFor(content);
    Check(viewportBox&&contentBox&&viewportBox->scrollWidth>=499.9f,
          L"horizontal overflow contributes to scrollWidth");
    const float initialContentX=contentBox?contentBox->rect.x:0;

    JavaScriptRuntime javascript(document);
    javascript.SetGeometryProvider([&](const std::shared_ptr<Node>& node){
        JavaScriptRuntime::NodeGeometry geometry;
        if(const auto* box=layout.BoxFor(node)){
            geometry.x=box->rect.x;geometry.y=box->rect.y;
            geometry.width=box->rect.width;geometry.height=box->rect.height;
            geometry.clientWidth=box->content.width;geometry.clientHeight=box->content.height;
            geometry.scrollWidth=box->scrollWidth;geometry.scrollHeight=box->scrollHeight;
        }
        return geometry;
    });
    std::wstring result;
    Check(javascript.Execute(L"const viewport = document.getElementById('viewport'); return viewport.scrollWidth >= 500 && viewport.clientWidth == 200;", &result, &error)&&result==L"true",
          L"DOM geometry exposes clientWidth and scrollWidth in CSS pixels");
    Check(javascript.Execute(L"viewport.scrollTo({ left: 120, top: 0 }); return viewport.scrollLeft;", &result, &error)&&result==L"120",
          L"scrollTo options update scrollLeft");
    Check(javascript.Execute(L"viewport.scrollBy({ left: 30 }); return viewport.scrollLeft;", &result, &error)&&result==L"150",
          L"scrollBy applies a relative horizontal offset");
    layout.Layout(600.0f, 300.0f, scale);
    contentBox=layout.BoxFor(content);
    Check(contentBox&&std::abs(contentBox->rect.x-(initialContentX-150.0f))<0.01f,
          L"scrollLeft translates overflow content on the horizontal axis");

    viewport->scrollLeft=0;
    layout.Layout(600.0f, 300.0f, scale);
    const auto* refreshed=layout.BoxFor(viewport);
    const float dipX=std::round((refreshed->rect.x+20.0f)*scale)/scale;
    const float dipY=std::round((refreshed->rect.y+72.0f)*scale)/scale;
    std::shared_ptr<Node> dragNode;float dragOffset=0;bool horizontal=false;
    Check(layout.BeginScrollbarInteraction(dipX,dipY,dragNode,dragOffset,horizontal)&&
              dragNode==viewport&&horizontal,
          L"the horizontal thumb is hit-testable using DPI-normalized coordinates");
    Check(layout.DragScrollbar(viewport,refreshed->rect.x+110.0f,dipY,dragOffset,true)&&
              viewport->scrollLeft>0,
          L"dragging the horizontal thumb updates scrollLeft");

    const float beforeWheel=viewport->scrollLeft;
    std::shared_ptr<Node> scrolled;
    Check(layout.ScrollAt(refreshed->content.x+100.0f,refreshed->content.y+20.0f,-120.0f,&scrolled,true)&&
              scrolled==viewport&&viewport->scrollLeft>beforeWheel,
          L"horizontal wheel input scrolls the nearest overflow container");
}

} // namespace

int wmain() {
    CheckScale(1.0f);
    CheckScale(1.5f);
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Horizontal scroll regression tests passed at 100% and 150% scaling\n";
    return 0;
}
