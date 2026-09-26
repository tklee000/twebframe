#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace TWebFrame::Internal;
namespace {
int failures = 0;
void Check(bool ok, const wchar_t* message) {
    if (!ok) { ++failures; std::wcerr << L"FAIL: " << message << std::endl; }
}
template<class F> double Time(F&& action) {
    const auto start = std::chrono::steady_clock::now();
    action();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
std::vector<float> Geometry(const LayoutEngine& layout) {
    std::vector<float> result;
    const auto visit=[&](const auto& self,const LayoutBox& box)->void {
        for(float value:{box.rect.x,box.rect.y,box.rect.width,box.rect.height,
            box.content.x,box.content.y,box.content.width,box.content.height,
            box.scrollWidth,box.scrollHeight})result.push_back(value);
        for(const auto& child:box.children)self(self,*child);
    };
    if(layout.Root())visit(visit,*layout.Root());
    return result;
}
void CheckFormattingRules(float scale) {
    Document document;
    StyleSheet styles;
    document.Parse(LR"(<style>
        *{box-sizing:border-box;margin:0;padding:0}
        body{font:11px/17px monospace}
        .frame{width:80%;padding:2% 3vw}
        .row{display:grid;grid-template-columns:46px minmax(max-content,1fr);white-space:pre;min-height:17px}
        .row>span{border-right:1px solid #ddd;padding:0 8px}
        .wrap{white-space:pre-wrap;width:40px}
        textarea{width:40px;height:17px;white-space:pre;overflow:auto;border:0;font:11px/17px monospace}
        </style><div class='frame'><div class='row'><span>12345</span><span>Long preserved line with spaces and a tab&#9;end</span></div>
        <div class='wrap'>a long line that must still wrap</div>
        <textarea>a&#13;&#10;b&#13;&#10;</textarea></div>)");
    styles.Parse(document.StyleText());
    document.QuerySelector(L"textarea")->SetAttribute(L"value",L"a\r\nb\r\n");
    LayoutEngine layout(document,styles);
    layout.Layout(800,400,scale);
    const auto row=document.QuerySelector(L".row");
    Check(std::abs(layout.BoxFor(row)->rect.height-17)<.01f,L"pre grid rows retain their authored line height");
    Check(layout.BoxFor(document.QuerySelector(L".wrap")->children.front())->rect.height>17,L"pre-wrap still wraps long text");
    Check(std::abs(layout.BoxFor(document.QuerySelector(L"textarea"))->scrollHeight-51)<.01f,
        L"pre CRLF and trailing segment breaks retain empty lines");
    const auto original=Geometry(layout);
    for(float targetScale:{scale==1?1.5f:1.0f,scale}){
        layout.Relayout(620,400,targetScale);
        const auto incremental=Geometry(layout);
        LayoutEngine fresh(document,styles);fresh.Layout(620,400,targetScale);
        Check(incremental==Geometry(fresh),L"font and edge caches agree with fresh layout after DPI and viewport changes");
    }
    layout.Relayout(800,400,scale);
    Check(original==Geometry(layout),L"100%/150% round trip preserves fractional CSS geometry");
    const auto textCell=row->children.back();
    const float oldWidth=layout.BoxFor(textCell)->rect.width;
    textCell->SetAttribute(L"style",L"font-size:22px;letter-spacing:2px;tab-size:8");
    layout.Layout(800,400,scale);
    Check(layout.BoxFor(textCell)->rect.width>oldWidth,L"changed shaping properties invalidate cached text widths");
}

void CheckManagedGraph() {
    Document document;document.Parse(L"<main></main>");
    JavaScriptRuntime js(document);
    std::wstring result,error;
    Check(js.Execute(LR"(
        let saved=[]; let hits=0;
        for(let i=0;i<12000;i++) {
            const node=document.createElement('button');
            node.addEventListener('click',()=>{hits++;});
            saved.push(node);
        }
        for(let i=0;i<24000;i++){const temporary={index:i};}
        saved[0].click(); saved[11999].click();
        return hits+':'+saved.length+':'+String(0)+':'+String(-42)+':'+String(123456789);
    )",&result,&error)&&result==L"2:12000:0:-42:123456789",
        L"amortized registry pruning retains referenced detached nodes and callbacks");
    js.Clear();
    Check(js.Execute(L"return typeof saved;",&result,&error)&&result==L"undefined",
        L"runtime reset releases and resets the managed graph");
}
void Run(size_t rows, float scale) {
    Document document;
    StyleSheet styles;
    document.Parse(LR"(<style>
        *{box-sizing:border-box;margin:0;padding:0}
        body{font:11px/1.52 Consolas,monospace}
        section{height:300px;width:640px;overflow:auto}
        article{display:grid;grid-template-columns:46px 46px minmax(max-content,1fr);min-height:17px;white-space:pre}
        article span{padding:0 8px;border-right:1px solid #ddd}
        article.added{background:#ddeee4}
        footer{height:30px}
        </style><section></section><footer>End</footer>)");
    styles.Parse(document.StyleText());
    JavaScriptRuntime js(document);
    std::wstring error, result;
    Check(js.Load(LR"(
        const panel=document.querySelector('section');
        let records=[];
        let source='';
        function populate(count) {
            records=[];
            for(let i=0;i<count;i++) records.push({index:i,text:'Entry '+i+' English \uD55C\uAD6D\uC5B4 \u65E5\u672C\u8A9E \u0939\u093F\u0928\u094D\u0926\u0940 \u0627\u0644\u0639\u0631\u0628\u064A\u0629'});
        }
        function render() {
            source=records.map(row=>`<article class="added"><span>${row.index}</span><span>${row.index+1}</span><span>${row.text}</span></article>`).join('');
        }
        function insert() { panel.innerHTML=source; }
    )", &error), L"large-content script compiles");
    const auto execute = [&](const std::wstring& script) {
        Check(js.Execute(script, &result, &error), L"large-content script executes");
        if (!error.empty()) std::wcerr << error << std::endl;
    };
    const double populate = Time([&] { execute(L"populate("+std::to_wstring(rows)+L");"); });
    const double render = Time([&] { execute(L"render();"); });
    const double insert = Time([&] { execute(L"insert();"); });
    LayoutEngine layout(document, styles);
    const double firstLayout = Time([&] { layout.Layout(800,400,scale); });
    const double rebuild = Time([&] { layout.Layout(800,400,scale); });
    const auto panel = document.QuerySelector(L"section");
    const auto* box = layout.BoxFor(panel);
    Check(box && box->scrollHeight >= rows*17-1, L"all grid rows contribute to scroll height");
    Check(panel->children.size()==rows,L"large DOM insertion retains every row");
    Check(panel->children.back()->InnerText().find(L"\uD55C\uAD6D\uC5B4")!=std::wstring::npos,
        L"multilingual text is preserved without substitutions");
    const double events = Time([&] {
        for (int i=0;i<100;++i) js.DispatchNodeEvent(panel,L"pointermove");
    });
    const double scroll = Time([&] {
        for (int i=0;i<100;++i) {
            panel->scrollTop=static_cast<float>((i+1)*29);
            Check(layout.SyncScroll(panel),L"large-content scroll synchronizes");
        }
    });
    const auto scrolled=Geometry(layout);
    layout.Layout(800,400,scale);
    Check(scrolled==Geometry(layout),L"large-content scroll geometry agrees with a complete rebuild");
    std::wcout << rows << L',' << scale << L',' << populate << L',' << render << L',' << insert
        << L',' << firstLayout << L',' << rebuild << L',' << events/100 << L',' << scroll/100 << std::endl;
}
}
int wmain() {
    std::wcout << std::fixed << std::setprecision(3);
    std::wcout << L"rows,scale,populate_ms,render_ms,insert_ms,layout_ms,rebuild_ms,event_ms,scroll_ms" << std::endl;
    CheckManagedGraph();
    for (float scale : {1.0f,1.5f}) {
        CheckFormattingRules(scale);
        for (size_t rows : {1000u,5000u}) Run(rows,scale);
    }
    return failures ? 1 : 0;
}
