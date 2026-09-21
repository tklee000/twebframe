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

void Check(bool condition,const wchar_t* message){
    if(!condition){std::wcerr<<L"FAIL: "<<message<<L'\n';++failures;}
}

void CheckNear(float actual,float expected,const wchar_t* message){
    if(std::abs(actual-expected)>=0.01f){
        std::wcerr<<L"FAIL: "<<message<<L" (actual "<<actual<<L", expected "<<expected<<L")\n";
        ++failures;
    }
}

const LayoutBox* Box(const LayoutEngine& layout,const Document& document,const wchar_t* id){
    return layout.BoxFor(document.GetElementById(id));
}

void CheckScale(float scale){
    const wchar_t* html=LR"HTML(
        <style>
            * { box-sizing: border-box; margin: 0; padding: 0; }
            table { border-collapse: collapse; table-layout: fixed; }
            #html-table { width: 360px; }
            #html-table tr { height: 30px; }
            #zero-table { width: 240px; }
            #zero-table tr { height: 20px; }
            #css-table { display: table; width: 300px; table-layout: fixed; }
            .css-group { display: table-row-group; }
            .css-row { display: table-row; height: 25px; }
            .css-cell { display: table-cell; }
            #invalid-table { width: 200px; }
            #invalid-table tr { height: 20px; }
        </style>
        <table id="html-table"><tbody>
            <tr><td id="html-a">A</td><td id="html-b" colspan="2">B</td></tr>
            <tr><td id="html-c">C</td><td id="html-d">D</td></tr>
            <tr><td id="html-e" colspan="2">E</td><td id="html-f">F</td></tr>
        </tbody></table>
        <table id="zero-table">
            <tbody>
                <tr><td id="zero-span" rowspan="0">Z</td><td>Z1</td></tr>
                <tr><td id="zero-last">Z2</td></tr>
            </tbody>
            <tfoot><tr><td id="footer-first">F1</td><td id="footer-last">F2</td></tr></tfoot>
        </table>
        <div id="css-table"><div class="css-group">
            <div class="css-row"><div id="css-a" class="css-cell" rowspan="2">A</div><div id="css-b" class="css-cell" colspan="2">B</div></div>
            <div class="css-row"><div id="css-c" class="css-cell">C</div><div id="css-d" class="css-cell">D</div></div>
        </div></div>
        <table id="invalid-table"><tbody><tr>
            <td id="invalid-zero" colspan="0">I1</td><td id="invalid-text" colspan="nope">I2</td>
        </tr></tbody></table>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html,&error),error.c_str());
    StyleSheet styles;
    Check(styles.Parse(document.StyleText(),&error),error.c_str());
    JavaScriptRuntime javascript(document);
    std::wstring result;
    Check(javascript.Execute(
        L"document.getElementById('html-a').rowSpan = 2; return document.getElementById('html-a').rowSpan;",
        &result,&error)&&result==L"2",L"rowSpan reflects the lowercase HTML rowspan attribute");

    LayoutEngine layout(document,styles);
    layout.Layout(500.0f,400.0f,scale);

    const auto* htmlA=Box(layout,document,L"html-a");
    const auto* htmlB=Box(layout,document,L"html-b");
    const auto* htmlC=Box(layout,document,L"html-c");
    const auto* htmlD=Box(layout,document,L"html-d");
    const auto* htmlE=Box(layout,document,L"html-e");
    const auto* htmlF=Box(layout,document,L"html-f");
    Check(htmlA&&htmlB&&htmlC&&htmlD&&htmlE&&htmlF,L"the HTML span fixture is laid out");
    if(htmlA&&htmlB&&htmlC&&htmlD&&htmlE&&htmlF){
        CheckNear(htmlA->rect.width,120.0f,L"a rowspan cell keeps one column width");
        CheckNear(htmlA->rect.height,60.0f,L"rowspan combines both row heights");
        CheckNear(htmlB->rect.width,240.0f,L"colspan combines adjacent column widths");
        CheckNear(htmlC->rect.x,htmlA->rect.x+htmlA->rect.width,
                  L"the next row skips a column occupied by rowspan");
        CheckNear(htmlD->rect.x,htmlC->rect.x+htmlC->rect.width,
                  L"cells after a rowspan occupy the next free grid slots");
        CheckNear(htmlE->rect.width,240.0f,L"a later colspan uses the shared column grid");
        CheckNear(htmlF->rect.x,htmlE->rect.x+htmlE->rect.width,
                  L"a cell follows a later colspan without overlap");
        const auto physicalEdge=[](float value,float deviceScale){
            return static_cast<long>(std::lround(value*deviceScale));
        };
        Check(physicalEdge(htmlA->rect.x+htmlA->rect.width,scale)==
              physicalEdge(htmlC->rect.x,scale),
              L"rowspan column edges stay contiguous on the device-pixel grid");
        Check(physicalEdge(htmlE->rect.x+htmlE->rect.width,scale)==
              physicalEdge(htmlF->rect.x,scale),
              L"colspan edges stay contiguous on the device-pixel grid");
        const float hitX=std::round((htmlC->rect.x+htmlC->rect.width/2)*scale)/scale;
        const float hitY=std::round((htmlC->rect.y+htmlC->rect.height/2)*scale)/scale;
        Check(layout.HitTest(hitX,hitY)==document.GetElementById(L"html-c"),
              L"spanned table geometry remains hit-testable at the active DPI");
    }

    const auto* zeroSpan=Box(layout,document,L"zero-span");
    const auto* zeroLast=Box(layout,document,L"zero-last");
    const auto* footerFirst=Box(layout,document,L"footer-first");
    const auto* footerLast=Box(layout,document,L"footer-last");
    Check(zeroSpan&&zeroLast&&footerFirst&&footerLast,L"the row-group fixture is laid out");
    if(zeroSpan&&zeroLast&&footerFirst&&footerLast){
        CheckNear(zeroSpan->rect.height,zeroLast->rect.height*2.0f,
                  L"rowspan zero extends to the end of its row group");
        CheckNear(zeroLast->rect.x,zeroSpan->rect.x+zeroSpan->rect.width,
                  L"rowspan zero reserves its column in subsequent body rows");
        CheckNear(footerFirst->rect.x,zeroSpan->rect.x,
                  L"rowspan zero does not cross into a footer row group");
        CheckNear(footerFirst->rect.width,footerLast->rect.width,
                  L"tfoot uses the common table row-group rules");
    }

    const auto* cssA=Box(layout,document,L"css-a");
    const auto* cssB=Box(layout,document,L"css-b");
    const auto* cssC=Box(layout,document,L"css-c");
    const auto* cssD=Box(layout,document,L"css-d");
    Check(cssA&&cssB&&cssC&&cssD,L"the CSS display table fixture is laid out");
    if(cssA&&cssB&&cssC&&cssD){
        CheckNear(cssA->rect.width,100.0f,L"CSS table-cell rowspan uses one track");
        CheckNear(cssA->rect.height,50.0f,L"CSS table-cell rowspan combines row tracks");
        CheckNear(cssB->rect.width,200.0f,L"CSS table-cell colspan combines column tracks");
        CheckNear(cssC->rect.x,cssA->rect.x+cssA->rect.width,
                  L"CSS table rows honor occupied rowspan tracks");
        CheckNear(cssD->rect.x,cssC->rect.x+cssC->rect.width,
                  L"CSS table cells use the next available track");
    }

    const auto* invalidZero=Box(layout,document,L"invalid-zero");
    const auto* invalidText=Box(layout,document,L"invalid-text");
    Check(invalidZero&&invalidText,L"the invalid span fixture is laid out");
    if(invalidZero&&invalidText){
        CheckNear(invalidZero->rect.width,100.0f,L"colspan zero falls back to one column");
        CheckNear(invalidText->rect.width,100.0f,L"an invalid colspan falls back to one column");
    }
}

} // namespace

int wmain(){
    CheckScale(1.0f);
    CheckScale(1.5f);
    if(failures){std::wcerr<<failures<<L" test(s) failed\n";return 1;}
    std::wcout<<L"Table span regression tests passed at 100% and 150% scaling\n";
    return 0;
}
