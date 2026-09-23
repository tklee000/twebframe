#include <TWebFrame/TWebFrame.h>

#include "DOM.h"
#include "CSS.h"
#include "JavaScript.h"
#include "Layout.h"
#include "TextInput.h"

#include <windows.h>
#include <imm.h>
#include <commctrl.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

using namespace TWebFrame::Internal;

namespace {
int failures = 0;
volatile LONG firstChanceCppExceptions = 0;
LONG CALLBACK CountFirstChanceCppExceptions(EXCEPTION_POINTERS* exception) {
    if(exception&&exception->ExceptionRecord&&
       exception->ExceptionRecord->ExceptionCode==0xe06d7363UL)
        InterlockedIncrement(&firstChanceCppExceptions);
    return EXCEPTION_CONTINUE_SEARCH;
}
LRESULT resizeHostHit=HTCLIENT;
WPARAM resizeHostButtonDown=0;
LRESULT CALLBACK ResizeHostWindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    if(message==WM_NCHITTEST)return resizeHostHit;
    if(message==WM_NCLBUTTONDOWN){resizeHostButtonDown=wParam;return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
struct ConstantHash { std::size_t operator()(int) const { return 1; } };
void Check(bool condition, const wchar_t* message) {
    if (!condition) { std::wcerr << L"FAIL: " << message << L"\n"; ++failures; }
}
const LayoutBox* FindLayout(const LayoutBox* box, const std::wstring& id, const std::wstring& className=L"") {
    if (!box) return nullptr;
    if ((!id.empty() && box->node->Attribute(L"id") == id) ||
        (!className.empty() && box->node->HasClass(className))) return box;
    for (const auto& child : box->children) if (const auto* found=FindLayout(child.get(),id,className)) return found;
    return nullptr;
}
const LayoutBox* FindPseudo(const LayoutBox* box, const std::wstring& pseudo) {
    if (!box) return nullptr;
    if (box->pseudo == pseudo) return box;
    for (const auto& child : box->children)
        if (const auto* found=FindPseudo(child.get(),pseudo)) return found;
    return nullptr;
}

struct FrameStats {
    double mean=0;
    double p50=0;
    double p95=0;
    double p99=0;
    double maximum=0;
};

FrameStats SummarizeFrames(std::vector<double> samples) {
    FrameStats result;
    if(samples.empty())return result;
    std::sort(samples.begin(),samples.end());
    const auto percentile=[&](double value){
        const size_t index=std::min(samples.size()-1,
            static_cast<size_t>(std::ceil(value*static_cast<double>(samples.size())))-1);
        return samples[index];
    };
    result.mean=std::accumulate(samples.begin(),samples.end(),0.0)/samples.size();
    result.p50=percentile(0.50);result.p95=percentile(0.95);result.p99=percentile(0.99);
    result.maximum=samples.back();return result;
}

bool MeasureFrames(HWND window,size_t sampleCount,
                   const std::function<bool(size_t)>& action,FrameStats& result) {
    using Clock=std::chrono::steady_clock;
    constexpr size_t warmupCount=8;
    for(size_t index=0;index<warmupCount;++index){
        if(!action(index))return false;
        UpdateWindow(window);
    }
    std::vector<double> samples;samples.reserve(sampleCount);
    for(size_t index=0;index<sampleCount;++index){
        const auto start=Clock::now();
        if(!action(index+warmupCount))return false;
        UpdateWindow(window);
        samples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
    }
    result=SummarizeFrames(std::move(samples));return true;
}

int RunFrameBenchmark() {
    using Clock=std::chrono::steady_clock;
    constexpr size_t nodeCount=10000;
    constexpr size_t sampleCount=120;
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|WS_VISIBLE,
        -10000,-10000,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!host){std::wcerr<<L"benchmark host creation failed\n";return 1;}
    RECT bounds{0,0,1280,720};auto view=TWebFrame::View::Create(host,bounds);
    if(!view){DestroyWindow(host);std::wcerr<<L"benchmark view creation failed\n";return 1;}
    std::wstring html;
    html.reserve(nodeCount*72);
    html+=LR"HTML(<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;font:14px Segoe UI}#viewport{height:680px;overflow:auto}.item{height:22px;padding:2px 8px;border-bottom:1px solid #eee;color:#222}.item:hover,.item.hot{color:#b91c1c;background:#fef2f2}</style><div id="viewport">)HTML";
    for(size_t index=0;index<nodeCount;++index)
        html+=L"<div class='item' id='row-"+std::to_wstring(index)+L"'>Row "+
            std::to_wstring(index)+L"</div>";
    html+=L"</div>";
    const auto loadStart=Clock::now();
    if(!view->NavigateToString(html)){std::wcerr<<view->LastError()<<L'\n';view.reset();DestroyWindow(host);return 1;}
    UpdateWindow(view->Window());
    const double initialLoad=std::chrono::duration<double,std::milli>(Clock::now()-loadStart).count();

    FrameStats hover,style,scroll;
    const bool hoverOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const int y=11+static_cast<int>(index%28)*22;
        SendMessageW(view->Window(),WM_MOUSEMOVE,0,MAKELPARAM(80,y));return true;
    },hover);
    SendMessageW(view->Window(),WM_MOUSELEAVE,0,0);UpdateWindow(view->Window());
    std::wstring error;
    const bool styleOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const auto id=std::to_wstring(index%nodeCount);
        return view->ExecuteScript(L"document.getElementById('row-"+id+L"').classList.toggle('hot');",
                                   nullptr,&error);
    },style);
    view->ExecuteScript(L"document.getElementById('viewport').scrollTop=0;",nullptr,&error);
    UpdateWindow(view->Window());
    POINT wheelPoint{100,100};ClientToScreen(view->Window(),&wheelPoint);
    const bool scrollOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
            MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
    },scroll);

    std::wcout<<std::fixed<<std::setprecision(3)
        <<L"TWebFrame 10k-node frame benchmark (milliseconds)\n"
        <<L"initial_load_and_paint="<<initialLoad<<L"\n"
        <<L"scenario,samples,mean,p50,p95,p99,max\n";
    const auto print=[&](const wchar_t* name,const FrameStats& value){
        std::wcout<<name<<L','<<sampleCount<<L','<<value.mean<<L','<<value.p50<<L','
                  <<value.p95<<L','<<value.p99<<L','<<value.maximum<<L'\n';
    };
    print(L"hover",hover);print(L"style_toggle",style);print(L"wheel_scroll",scroll);
    view.reset();DestroyWindow(host);
    if(!hoverOk||!styleOk||!scrollOk){
        std::wcerr<<L"benchmark action failed: "<<error<<L'\n';return 1;
    }
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    const HRESULT comInitialization=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const bool uninitializeCom=SUCCEEDED(comInitialization);
    if(argc>1&&_wcsicmp(argv[1],L"--benchmark")==0){
        const int result=RunFrameBenchmark();if(uninitializeCom)CoUninitialize();return result;
    }
    FastMap<int, std::wstring, ConstantHash> fastMap;
    for (int i = 0; i < 64; ++i) fastMap[i] = std::to_wstring(i);
    fastMap[17] = L"updated";
    Check(fastMap.size() == 64 && fastMap.find(17) != fastMap.end() &&
          fastMap.find(17)->second == L"updated" && fastMap.count(99) == 0,
          L"custom map handles growth, collisions and updates");
    fastMap.clear(); fastMap[3] = L"reused";
    Check(fastMap.size() == 1 && fastMap.find(3)->second == L"reused",
          L"custom map can reuse cleared buckets");

    std::wstring error;
    Document queryDoc;
    Check(queryDoc.Parse(L"<body><section id='scope'><span class='first'></span><div><span class='deep'></span></div></section><span class='outside'></span></body>"),
          L"query optimization fixture parses");
    const auto queryScope=queryDoc.GetElementById(L"scope");
    Check(queryDoc.QuerySelector(L".first,.deep",queryScope)==queryScope->children.front(),
          L"querySelector preserves document-order early exit for selector groups");
    Check(queryDoc.QuerySelectorAll(L":scope > .first,.deep",queryScope).size()==2,
          L"compiled scoped selector groups preserve querySelectorAll behavior");
    queryScope->SetAttribute(L"DATA-MIXED",L"value");
    Check(queryScope->Attribute(L"data-mixed")==L"value"&&
          queryScope->Attribute(L"DATA-MIXED")==L"value",
          L"attribute fast path preserves case-insensitive lookup");
    queryScope->SetAttribute(L"class",L"alpha\tbeta  gamma");
    Check(queryScope->HasClass(L"beta")&&!queryScope->HasClass(L"bet"),
          L"class token scan handles whitespace without stream allocation");
    Check(queryDoc.QuerySelector(L"section.alpha")==queryScope,
          L"direct native class mutation updates the owning document candidate index");
    std::shared_ptr<Node> retainedAfterDocument;
    {
        Document ownerDoc;
        Check(ownerDoc.Parse(L"<body><div id='retained'></div></body>"),
              L"owner-document lifecycle fixture parses");
        retainedAfterDocument=ownerDoc.GetElementById(L"retained");
        Check(retainedAfterDocument&&retainedAfterDocument->ownerDocument==&ownerDoc,
              L"indexed nodes retain their owning document while connected");
    }
    Check(retainedAfterDocument&&retainedAfterDocument->ownerDocument==nullptr,
          L"document destruction disconnects retained nodes from candidate indexes");
    retainedAfterDocument->AddClass(L"safe-after-document");

    Document mutationDoc;
    Check(mutationDoc.Parse(L"<body><div id='scroll'><span></span></div></body>"),
          L"mutation classification fixture parses");
    JavaScriptRuntime mutationJs(mutationDoc);
    JavaScriptRuntime::Mutation lastMutation;int mutationNotifications=0;
    mutationJs.SetMutationSink([&](const JavaScriptRuntime::Mutation& mutation){
        lastMutation=mutation;++mutationNotifications;
    });
    Check(mutationJs.Execute(L"document.getElementById('scroll').scrollTop=5;",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Paint&&
          lastMutation.targets.size()==1&&lastMutation.targets.front()==mutationDoc.GetElementById(L"scroll"),
          L"scroll mutations remain paint-only and retain their target");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"const node=document.getElementById('scroll');node.scrollTop=8;node.style.color='red';",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Style,
          L"batched mutations retain the strongest invalidation level");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"document.getElementById('scroll').setAttribute('width','240');",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Layout,
          L"geometry-affecting attributes request layout without a tree mutation");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"const child=document.createElement('b');document.getElementById('scroll').appendChild(child);",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Tree,
          L"tree mutations request a layout-tree rebuild");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"document.getElementById('scroll').setAttribute('ARIA-LIVE','polite');",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.liveRegionMembershipChanged,
          L"aria-live membership changes are identified without a document-wide scan");
    const auto incrementalIndexStart=mutationDoc.FullReindexCount();
    Check(mutationJs.Execute(L"const indexedChild=document.createElement('i');indexedChild.id='incremental-id';document.getElementById('scroll').appendChild(indexedChild);",nullptr,&error)&&
          mutationDoc.GetElementById(L"incremental-id")!=nullptr&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique subtree insertion updates the ID index without a full document walk");
    Check(mutationJs.Execute(L"indexedChild.id='renamed-id';",nullptr,&error)&&
          !mutationDoc.GetElementById(L"incremental-id")&&mutationDoc.GetElementById(L"renamed-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique ID replacement updates the index incrementally");
    Check(mutationJs.Execute(L"indexedChild.remove();",nullptr,&error)&&
          !mutationDoc.GetElementById(L"renamed-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique subtree removal updates the ID index incrementally");
    Check(mutationJs.Execute(L"const candidateChild=document.createElement('article');candidateChild.className='candidate-old';document.getElementById('scroll').appendChild(candidateChild);",nullptr,&error)&&
          mutationDoc.QuerySelector(L"article.candidate-old")==mutationDoc.QuerySelector(L".candidate-old")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree insertion updates tag and class candidate indexes incrementally");
    Check(mutationJs.Execute(L"candidateChild.classList.remove('candidate-old');candidateChild.classList.add('candidate-new');",nullptr,&error)&&
          !mutationDoc.QuerySelector(L".candidate-old")&&
          mutationDoc.QuerySelector(L"article.candidate-new")!=nullptr&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"classList mutations update class candidates without a full reindex");
    Check(mutationJs.Execute(L"candidateChild.setAttribute('class','candidate-set');",nullptr,&error)&&
          !mutationDoc.QuerySelector(L".candidate-new")&&mutationDoc.QuerySelector(L".candidate-set")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"class attribute replacement updates class candidates incrementally");
    Check(mutationJs.Execute(L"candidateChild.remove();",nullptr,&error)&&
          !mutationDoc.QuerySelector(L"article.candidate-set")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree removal clears tag and class candidate indexes incrementally");
    Check(mutationJs.Execute(L"const duplicateA=document.createElement('b');const duplicateB=document.createElement('b');duplicateA.id='duplicate-id';duplicateB.id='duplicate-id';document.getElementById('scroll').append(duplicateA,duplicateB);",nullptr,&error)&&
          mutationDoc.GetElementById(L"duplicate-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart+1,
          L"duplicate IDs conservatively fall back to one full reindex");
    const auto duplicateMatches=mutationDoc.QuerySelectorAll(L"#duplicate-id");
    Check(duplicateMatches.size()==2&&mutationDoc.QuerySelector(L"#duplicate-id")==duplicateMatches.front(),
          L"duplicate-ID candidate fallback preserves first-in-document query order");

    Document scrollSyncDoc;
    Check(scrollSyncDoc.Parse(L"<body><div id='scroller'><div id='scroll-content'></div></div></body>"),
          L"paint-only scroll fixture parses");
    StyleSheet scrollSyncCss;
    Check(scrollSyncCss.Parse(L"#scroller{width:160px;height:40px;overflow:auto}#scroll-content{height:200px}"),
          L"paint-only scroll styles parse");
    LayoutEngine scrollSyncLayout(scrollSyncDoc,scrollSyncCss);scrollSyncLayout.Layout(240,120);
    const auto scroller=scrollSyncDoc.GetElementById(L"scroller");
    const auto scrollContent=scrollSyncDoc.GetElementById(L"scroll-content");
    const float initialContentY=scrollSyncLayout.BoxFor(scrollContent)->rect.y;
    scroller->scrollTop=30;
    Check(scrollSyncLayout.SyncScroll(scroller)&&
          std::abs(scrollSyncLayout.BoxFor(scrollContent)->rect.y-(initialContentY-30))<0.01f&&
          !scrollSyncLayout.SyncScroll(scroller),
          L"paint-only DOM scrolling translates existing layout boxes exactly once");

    Document switchDoc;Check(switchDoc.Parse(L"<body></body>",&error),L"switch JavaScript fixture parses");
    JavaScriptRuntime switchJs(switchDoc);std::wstring switchResult;
    Check(switchJs.Load(L"function choose(value){let result='';switch(value){case 'a':result+='a';break;default:result+='d';case 'b':result+='b';}return result;}",&error),error.c_str());
    Check(switchJs.Execute(L"return choose('a')+'|'+choose('b')+'|'+choose('x');",&switchResult,&error),error.c_str());
    Check(switchResult==L"a|b|db",L"switch supports matching, break, default and fallthrough");
    Document semanticsDoc;Check(semanticsDoc.Parse(L"<body></body>",&error),L"JavaScript semantics fixture parses");
    JavaScriptRuntime semanticsJs(semanticsDoc);std::wstring semanticsResult;
    semanticsJs.SetViewportSize(1280,720);
    Check(semanticsJs.Execute(
        L"return innerWidth+'|'+innerHeight+'|'+window.innerWidth+'|'+window.innerHeight;",
        &semanticsResult,&error)&&semanticsResult==L"1280|720|1280|720",
        L"viewport dimensions are exposed through browser window globals");
    Check(semanticsJs.Load(
        L"let exceptionLog='';"
        L"function throwAcrossFrame(){throw {message:'boom'};}"
        L"function catchAcrossFrame(){try{throwAcrossFrame();exceptionLog+='bad';}catch(error){exceptionLog+=error.message;}finally{exceptionLog+=':finally';}return exceptionLog;}"
        L"function finallyOverridesReturn(){try{return 'try';}finally{return 'finally';}}"
        L"function loopFinally(){let value='';for(let i=0;i<3;i++){try{if(i===1)continue;if(i===2)break;value+=i;}finally{value+='f';}}return value;}"
        L"let thrownIdentity=new TypeError('typed');let sameThrown=false;try{throw thrownIdentity;}catch(error){sameThrown=error===thrownIdentity&&error.name==='TypeError'&&error.message==='typed'&&error.stack==='TypeError: typed';}"
        L"let catchShadow='outer';let capturedCatch;try{throw 'inner';}catch(catchShadow){capturedCatch=()=>catchShadow;}",
        &error),error.c_str());
    Check(semanticsJs.Execute(L"return catchAcrossFrame()+'|'+finallyOverridesReturn()+'|'+loopFinally()+'|'+sameThrown+'|'+catchShadow+'|'+capturedCatch();",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"boom:finally|finally|0fff|true|outer|inner",
          L"VM exceptions preserve values and catch scope across calls while abrupt completions run finally");
    Check(semanticsJs.Execute(
        L"let compound=3;compound*=4;compound/=2;compound%=5;compound**=3;"
        L"let logicalRuns=0;let truthy='keep';truthy||=(++logicalRuns);let falsy=0;falsy||=7;"
        L"let enabled=true;enabled&&='yes';let stopped=false;stopped&&=(++logicalRuns);"
        L"let doValues=[];let doIndex=0;do{doIndex++;if(doIndex===2)continue;doValues.push(doIndex);}while(doIndex<3);"
        L"let ownKeys=[];for(const key in {alpha:1,beta:2})ownKeys.push(key);let reused='';for(reused in {gamma:3}){}"
        L"return compound+'|'+(+\"8\")+'|'+truthy+'|'+falsy+'|'+enabled+'|'+logicalRuns+'|'+doValues.join(',')+'|'+ownKeys.length+'|'+ownKeys.includes('alpha')+'|'+ownKeys.includes('beta')+'|'+reused;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"1|8|keep|7|yes|0|1,3|2|true|true|gamma",
          L"basic compound and logical assignments, unary plus, do-while and for-in syntax execute correctly");
    Check(semanticsJs.Execute(
        L"let bits=5;bits&=3;bits|=8;bits^=1;bits<<=2;bits>>=1;bits>>>=2;"
        L"let precedence=1|2&4;let unsigned=-1>>>0;let inverted=~0;"
        L"let present='alpha' in {alpha:1};let second=1 in [4,5];let missing=2 in [4,5];"
        L"let voidSide=0;let voidResult=void (voidSide=7);"
        L"let sequence=0;let sequenceValue=(sequence=1,sequence+=2,sequence*2);"
        L"function gather(head,...rest){return head+':'+rest.join('-')+':'+arguments.length;}"
        L"function trailing(a,b,){return a+b;}let sparse=[1,,3,];"
        L"return bits+'|'+precedence+'|'+unsigned+'|'+inverted+'|'+present+'|'+second+'|'+missing+'|'+typeof voidResult+'|'+voidSide+'|'+sequence+'|'+sequenceValue+'|'+gather(...['a','b','c'],)+'|'+trailing(2,3,)+'|'+sparse.length+'|'+typeof sparse[1];",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"4|1|4294967295|-1|true|true|false|undefined|7|3|6|a:b-c:3|5|3|undefined",
          L"bitwise, shift, in and void operators plus rest, arguments, trailing commas and array holes execute correctly");
    Check(semanticsJs.Execute(
        L"function options({enabled=true,label='fallback'}={}){return enabled+'|'+label;}"
        L"return options()+'|'+options({enabled:false,label:'custom'});",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|fallback|false|custom",
          L"destructured parameters support property and whole-pattern defaults");
    Check(semanticsJs.Execute(
        L"return '<p>x</p>'.replace(/^<([a-z]+)/i,'<$1 data-kind=\"block\"');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"<p data-kind=\"block\">x</p>",
          L"regular-expression replacement strings expand capture references");
    Check(semanticsJs.Execute(
        L"const marker=`\\u0000${7}\\u0000`;"
        L"return '\\u0041\\x42'+'|'+marker.replace(/\\u0000(\\d+)\\u0000/g,(_,index)=>`token-${index}`);",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"AB|token-7",
          L"string, template and regular-expression literals decode JavaScript Unicode escapes");
    const auto regexExceptionHandler=AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
    InterlockedExchange(&firstChanceCppExceptions,0);
    Check(semanticsJs.Execute(
        L"const localized=['&File','&\\uD30C\\uC77C','&\\u65E5\\u672C','&\\u0424\\u0430\\u0439\\u043B','&\\u6587\\u4EF6'];"
        L"return localized.map((label)=>label.replace(/&(?=\\p{L})/u,'')).join('|');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"File|\uD30C\uC77C|\u65E5\u672C|\u0424\u0430\u0439\u043B|\u6587\u4EF6",
          L"Unicode Letter property escapes match multilingual menu labels");
    Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
          L"supported Unicode property escapes compile without first-chance C++ exceptions");
    if(regexExceptionHandler)RemoveVectoredExceptionHandler(regexExceptionHandler);
    Check(semanticsJs.Execute(
        L"const fence=new RegExp('^ {0,3}```');const insensitive=RegExp('alpha','i');"
        L"const clone=new RegExp(insensitive);"
        L"return fence.test('```powershell')+'|'+fence.test('text')+'|'+clone.test('ALPHA');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|false|true",
          L"RegExp constructor creates dynamic expressions and clones existing expressions");
    Check(semanticsJs.Execute(
        L"let promiseOrder=[];"
        L"Promise.resolve(1).then((value)=>{promiseOrder.push('then'+value);return value+1;}).then((value)=>promiseOrder.push('chain'+value));"
        L"Promise.resolve({then:(resolve)=>resolve(7)}).then((value)=>promiseOrder.push('thenable'+value));"
        L"new Promise((resolve)=>resolve(3)).then((value)=>promiseOrder.push('constructed'+value));"
        L"Promise.reject(new Error('rejected')).catch((error)=>promiseOrder.push(error.message));"
        L"queueMicrotask(()=>promiseOrder.push('queued'));promiseOrder.push('sync');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return promiseOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"sync,then1,thenable7,constructed3,rejected,queued,chain2",
          L"Promise settlement, thenable assimilation and recursive microtask checkpoints preserve order");
    Check(semanticsJs.Execute(
        L"let promiseFinally=[];Promise.resolve('ok').finally(()=>promiseFinally.push('fulfilled-finally')).then((value)=>promiseFinally.push(value));Promise.reject('bad').finally(()=>promiseFinally.push('rejected-finally')).catch((reason)=>promiseFinally.push(reason));",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return promiseFinally.join(',');",&semanticsResult,&error)&&semanticsResult==L"fulfilled-finally,rejected-finally,ok,bad",
          L"Promise finally waits for its callback and preserves fulfillment values and rejection reasons");
    Check(semanticsJs.Execute(
        L"let aggregateOrder=[];"
        L"Promise.all([Promise.resolve(1),2]).then((values)=>aggregateOrder.push('all'+values.join('-')));"
        L"Promise.allSettled([Promise.resolve('yes'),Promise.reject('no')]).then((values)=>aggregateOrder.push(values[0].status+'-'+values[1].reason));"
        L"Promise.any([Promise.reject('first'),Promise.resolve('winner')]).then((value)=>aggregateOrder.push('any-'+value));",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return aggregateOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"all1-2,fulfilled-no,any-winner",
          L"Promise aggregate combinators retain input order and settlement semantics");
    Check(semanticsJs.Execute(
        L"let rejectionEvents=[];window.addEventListener('unhandledrejection',(event)=>rejectionEvents.push('unhandled-'+event.reason));window.addEventListener('rejectionhandled',()=>rejectionEvents.push('handled'));let lateRejection=Promise.reject('late');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"lateRejection.catch(()=>rejectionEvents.push('caught'));",nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return rejectionEvents.join(',');",&semanticsResult,&error)&&semanticsResult==L"unhandled-late,handled,caught",
          L"unhandled and subsequently handled promise rejections emit lifecycle events");
    Check(semanticsJs.Execute(
        L"let asyncOrder=[];"
        L"async function asyncTask(){asyncOrder.push('start');const value=await Promise.resolve(4);asyncOrder.push('after'+value);return value+1;}"
        L"async function asyncRecover(){try{await Promise.reject('no');return 'bad';}catch(error){return 'caught-'+error;}finally{asyncOrder.push('recover-finally');}}"
        L"function defaultFailure(){throw 'default';}async function asyncDefault(value=defaultFailure()){return value;}"
        L"const asyncArrow=async(value)=>await Promise.resolve(value+1);const asyncService={async load(value){return await asyncArrow(value);}};class AsyncWorker{async run(value){return await asyncService.load(value);}}"
        L"asyncTask().then((value)=>asyncOrder.push('done'+value));asyncRecover().then((value)=>asyncOrder.push(value));new AsyncWorker().run(8).then((value)=>asyncOrder.push('method'+value));asyncDefault().catch((reason)=>asyncOrder.push(reason));asyncOrder.push('sync');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return asyncOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"start,sync,after4,recover-finally,default,done5,caught-no,method9",
          L"async functions, arrows and methods suspend at await, resume from microtasks and route rejection through catch/finally");
    semanticsJs.SetResourceLoader([](const std::wstring& resource,std::wstring& body){if(resource!=L"fixture.json")return false;body=L"{\"value\":9}";return true;});
    Check(semanticsJs.Execute(
        L"let fetchValue='';async function loadFixture(){const response=await fetch('fixture.json');const data=await response.json();return response.ok+':'+data.value;}loadFixture().then((value)=>fetchValue=value);",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return fetchValue;",&semanticsResult,&error)&&semanticsResult==L"true:9",
          L"fetch and response.json compose as promises across consecutive await suspension points");
    Check(semanticsJs.Execute(
        L"let taskOrder=[];setTimeout(()=>taskOrder.push('timer'),0);Promise.resolve().then(()=>taskOrder.push('microtask'));taskOrder.push('script');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&semanticsResult==L"script,microtask",
          L"microtasks drain after the script task before timers");
    semanticsJs.RunTimers();
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&semanticsResult==L"script,microtask,timer",
          L"timer callbacks run as later tasks after the microtask checkpoint");
    Document eventDoc;Check(eventDoc.Parse(L"<button id='event-button'>go</button><button id='compat-button'>compat</button>",&error),L"event fixture parses");
    JavaScriptRuntime eventJs(eventDoc);std::wstring eventResult;bool focusRequested=false;
    eventJs.SetFocusSink([&](const std::shared_ptr<Node>& node){focusRequested=node==eventDoc.GetElementById(L"event-button");node->focused=true;});
    Check(eventJs.Load(L"let eventLog='',compatLog='';const b=document.getElementById('event-button');b.addEventListener('pointerdown',(e)=>{eventLog+=e.button+':'+e.detail;e.preventDefault();e.stopPropagation();});document.addEventListener('pointerdown',()=>eventLog+=':bubble');document.addEventListener('keydown',(e)=>eventLog+='|'+e.key+':'+e.ctrlKey+':'+e.shiftKey);const c=document.getElementById('compat-button');c.addEventListener('pointerdown',(e)=>compatLog+='pointer:'+e.button+':'+e.detail);c.addEventListener('mousedown',(e)=>compatLog+='|mouse:'+e.button+':'+e.detail);b.focus();",&error),error.c_str());
    JavaScriptRuntime::EventInit pointerInit;pointerInit.button=0;pointerInit.detail=2;
    Check(eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"event-button"),L"pointerdown",pointerInit),
          L"preventDefault is reported to the native event source");
    JavaScriptRuntime::EventInit keyInit;keyInit.key=L"b";keyInit.ctrlKey=true;keyInit.shiftKey=true;
    eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"event-button"),L"keydown",keyInit);
    Check(eventJs.Execute(L"return eventLog;",&eventResult,&error),error.c_str());
    Check(eventResult==L"0:2|b:true:true",L"pointer and keyboard metadata bubble with propagation controls");
    Check(!eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"compat-button"),L"pointerdown",pointerInit),
          L"an uncanceled pointer press keeps its native default action");
    Check(eventJs.Execute(L"return compatLog;",&eventResult,&error)&&eventResult==L"pointer:0:2|mouse:0:2",
          L"pointerdown produces the standard compatibility mousedown event");
    Check(focusRequested,L"programmatic focus is synchronized with the native view");
    Check(eventJs.Execute(L"let timerLog='';setTimeout(()=>timerLog='ran',0);",nullptr,&error),error.c_str());
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult.empty(),
          L"setTimeout remains asynchronous even with a zero delay");
    eventJs.RunTimers();
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult==L"ran",
          L"due timeout callbacks run through the generic timer queue");
    Check(semanticsJs.Execute(
        L"const sortable=[{name:'beta'},{name:'alpha'}];sortable.sort((left,right)=>left.name.localeCompare(right.name));"
        L"const source={nested:{value:1}};const copied=structuredClone(source);copied.nested.value=9;"
        L"const parsedDate=new Date('2024-01-02T03:04:05.006Z');const relative=new Intl.RelativeTimeFormat('en-US',{numeric:'always'}).format(-2,'hour');"
        L"return sortable.at(0).name+'|'+sortable.some((item)=>item.name==='beta')+'|'+[1,2].findIndex((item)=>item===2)+'|'+source.nested.value+'|'+parsedDate.toISOString()+'|'+Number.isNaN(new Date('invalid').getTime())+'|'+Number.isInteger(5)+'|'+Number.isInteger(5.5)+'|'+Number.isInteger('5')+'|'+relative;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"alpha|true|1|1|2024-01-02T03:04:05.006Z|true|true|false|false|2 hours ago",
          L"generic array, string, structured clone, Date and Intl APIs compose correctly");
    Check(semanticsJs.Execute(
        L"const splitLines='one\\r\\ntwo\\nthree'.split(/\\r?\\n/);"
        L"const splitOptional='alpha--beta-gamma'.split(/--?/);"
        L"const splitLimited='a--b-c'.split(/--?/,2);"
        L"const hunk='@@ -12,2 +34,5 @@ heading'.match(/^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@/);"
        L"return splitLines.slice(0,2).join('|')+'|'+splitLines.slice(-1).join('|')+'|'+splitOptional.join('|')+'|'+splitLimited.join('|')+'|'+hunk[1]+'|'+hunk[2]+'|'+hunk.index+'|'+'abc'.split().join(',')+'|'+'abc'.match(/z/);",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"one|two|three|alpha|beta|gamma|a|b|12|34|0|abc|null",
          L"Array slice and String split and match apply reusable collection and regular-expression semantics");
    Check(eventJs.Execute(L"let intervalRuns=0;const intervalId=setInterval(()=>{intervalRuns++;if(intervalRuns===2)clearInterval(intervalId);},0);",nullptr,&error),error.c_str());
    eventJs.RunTimers();eventJs.RunTimers();eventJs.RunTimers();
    Check(eventJs.Execute(L"return intervalRuns;",&eventResult,&error)&&eventResult==L"2",
          L"setInterval repeats until clearInterval removes the generic timer");
    Document modernDomDoc;Check(modernDomDoc.Parse(L"<div id='scope'><span class='direct'></span><div><span class='direct'></span></div></div><div id='choices'><label><input type='radio' checked></label><label><input type='radio'></label></div><dialog id='dialog'></dialog><form id='form'></form>",&error),L"modern DOM fixture parses");
    Check(modernDomDoc.QuerySelectorAll(L"#choices label:has(input:checked)").size()==1&&modernDomDoc.QuerySelectorAll(L"#choices label:nth-of-type(2)").size()==1,
          L"relational and type-index CSS selectors match reusable document state");
    JavaScriptRuntime modernDomJs(modernDomDoc);std::wstring modernDomResult;
    Check(modernDomJs.Load(
        L"const scope=document.getElementById('scope');const dialog=document.getElementById('dialog');const form=document.getElementById('form');let submitted=false,closed=false;form.addEventListener('submit',(event)=>{submitted=true;event.preventDefault();});dialog.addEventListener('close',()=>closed=true);dialog.showModal();const opened=dialog.open;dialog.close('accepted');form.requestSubmit();const direct=scope.querySelectorAll(':scope > .direct').length;",
        &error),error.c_str());
    Check(modernDomJs.Execute(L"return opened+'|'+dialog.open+'|'+dialog.returnValue+'|'+submitted+'|'+closed+'|'+direct;",&modernDomResult,&error),error.c_str());
    Check(modernDomResult==L"true|false|accepted|true|true|1",
          L"dialog, form submission and scoped selectors use reusable DOM behavior");
    Document dialogDisplayDoc;Check(dialogDisplayDoc.Parse(L"<body><dialog id='closed' class='modal'></dialog><dialog id='opened' class='modal' open></dialog><div id='hidden' hidden></div></body>",&error),L"dialog display fixture parses");
    StyleSheet dialogDisplayCss;Check(dialogDisplayCss.Parse(L".modal{width:300px}",&error),L"dialog display CSS parses");
    LayoutEngine dialogDisplayLayout(dialogDisplayDoc,dialogDisplayCss);dialogDisplayLayout.Layout(800,600);
    const auto* closedDialog=FindLayout(dialogDisplayLayout.Root(),L"closed");const auto* openedDialog=FindLayout(dialogDisplayLayout.Root(),L"opened");const auto* hiddenElement=FindLayout(dialogDisplayLayout.Root(),L"hidden");
    Check(closedDialog&&!closedDialog->visible&&openedDialog&&openedDialog->visible&&openedDialog->style.Is(L"position",L"fixed")&&hiddenElement&&!hiddenElement->visible,
          L"closed dialogs and hidden elements skip layout while open dialogs enter the fixed top layer");
    Check(eventJs.Execute(L"const timer= setTimeout(()=>timerLog='wrong',0);clearTimeout(timer);",nullptr,&error),error.c_str());
    eventJs.RunTimers();
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult==L"ran",
          L"clearTimeout removes a queued timeout callback");
    Document svgDoc;Check(svgDoc.Parse(L"<style>svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:2}</style><svg style='display:none'><symbol id='icon' viewBox='0 0 24 24'><path d='M2 2L22 22'/></symbol></svg><svg id='instance'><use href='#icon'></use></svg>",&error),L"SVG use fixture parses");
    StyleSheet svgCss;Check(svgCss.Parse(svgDoc.StyleText(),&error),L"SVG use CSS parses");
    LayoutEngine svgLayout(svgDoc,svgCss);svgLayout.Layout(100,100);
    const auto* svgInstance=svgLayout.BoxFor(svgDoc.GetElementById(L"instance"));
    Check(svgInstance&&std::lround(svgInstance->rect.width)==18&&std::lround(svgInstance->rect.height)==18,
          L"SVG symbol use instances participate in generic layout");
    Document svgVariableDoc;
    Check(svgVariableDoc.Parse(
          L"<style>:root{--surface:#10151d;--edge:#ff654a}</style><svg id='variable-svg'><circle id='variable-circle' fill='var(--surface)' stroke='var(--edge)'/></svg>",
          &error),L"SVG presentation variable fixture parses");
    StyleSheet svgVariableCss;Check(svgVariableCss.Parse(svgVariableDoc.StyleText(),&error),
          L"SVG presentation variable CSS parses");
    const auto svgVariableStyle=svgVariableCss.Compute(svgVariableDoc.GetElementById(L"variable-svg"));
    const auto circleVariableStyle=svgVariableCss.Compute(
          svgVariableDoc.GetElementById(L"variable-circle"),&svgVariableStyle);
    Check(circleVariableStyle.Get(L"fill")==L"#10151d"&&
          circleVariableStyle.Get(L"stroke")==L"#ff654a",
          L"SVG presentation attributes resolve inherited custom properties");
    Document cascadeDoc;
    Check(cascadeDoc.Parse(L"<style>.x{background-color:#111;margin-left:1px;flex-grow:7;gap:3px 4px}.x{background:#fff;margin:2px;flex:1}</style><div class='x'></div>", &error),
          L"shorthand cascade fixture parses");
    StyleSheet cascadeCss; Check(cascadeCss.Parse(cascadeDoc.StyleText(), &error), L"shorthand cascade CSS parses");
    const auto cascadeStyle=cascadeCss.Compute(cascadeDoc.QuerySelector(L".x"));
    Check(cascadeStyle.Get(L"background-color")==L"#fff"&&cascadeStyle.Get(L"margin-left")==L"2px"&&
          cascadeStyle.Get(L"flex-grow")==L"1"&&cascadeStyle.Get(L"row-gap")==L"3px"&&
          cascadeStyle.Get(L"column-gap")==L"4px",L"shorthands reset canonical longhands in cascade order");
    Check(StyleSheet::Color(L"color-mix(in srgb, #d9dee8 68%, transparent)")==0xadd9dee8u,
          L"sRGB color-mix preserves opaque channels while mixing alpha with transparent");
    Document borderMixDoc;
    Check(borderMixDoc.Parse(
          L"<style>:root{--edge:#252e3b}.row{border-bottom:1px solid color-mix(in srgb,var(--edge) 65%,transparent)}</style><div id='mixed-border' class='row'></div>",
          &error),L"functional border color shorthand fixture parses");
    StyleSheet borderMixCss;Check(borderMixCss.Parse(borderMixDoc.StyleText(),&error),
          L"functional border color shorthand CSS parses");
    const auto borderMixStyle=borderMixCss.Compute(borderMixDoc.GetElementById(L"mixed-border"));
    Check(borderMixStyle.Get(L"border-bottom-color")==
              L"color-mix(in srgb,#252e3b 65%,transparent)"&&
          StyleSheet::Color(borderMixStyle.Get(L"border-bottom-color"))==0xa6252e3bu,
          L"border shorthand keeps the complete color function and its alpha");
    Document borderScaleDoc;
    Check(borderScaleDoc.Parse(
          L"<style>body{margin:0}.probe{box-sizing:border-box;width:100px;height:20px;border:1px solid #fff}</style><div id='border-scale' class='probe'></div>",
          &error),L"device-scale border fixture parses");
    StyleSheet borderScaleCss;Check(borderScaleCss.Parse(borderScaleDoc.StyleText(),&error),
          L"device-scale border CSS parses");
    LayoutEngine borderScaleLayout(borderScaleDoc,borderScaleCss);borderScaleLayout.Layout(200,80,1.0f);
    const auto* borderScaleBox=borderScaleLayout.BoxFor(borderScaleDoc.GetElementById(L"border-scale"));
    Check(borderScaleBox&&std::abs((borderScaleBox->content.x-borderScaleBox->rect.x)-1.0f)<0.001f&&
          std::abs(borderScaleBox->rect.width-100.0f)<0.001f,
          L"one CSS-pixel border remains one physical pixel at 100% scale");
    borderScaleLayout.Relayout(200,80,1.5f);
    borderScaleBox=borderScaleLayout.BoxFor(borderScaleDoc.GetElementById(L"border-scale"));
    Check(borderScaleBox&&std::abs((borderScaleBox->content.x-borderScaleBox->rect.x)-(2.0f/3.0f))<0.001f&&
          std::abs(borderScaleBox->rect.width-100.0f)<0.001f,
          L"one CSS-pixel border snaps to one physical pixel at 150% scale");

    Document basicCssDoc;
    Check(basicCssDoc.Parse(
        L"<style>body{margin:0}"
        L"[data-code^='prefix' i]{width:41px}[data-code$='value']{height:22px}"
        L"[data-code*='fix-v']{margin-left:3px}[data-tags~='hot']{padding-left:4px}"
        L"[lang|='en']{padding-right:5px}#selector-anchor ~ [data-code]{background:center/cover no-repeat teal}"
        L"#styled{width:10px;height:10px;font:italic 700 12px/1.5 'Segoe UI';border:4px solid rebeccapurple;border-style:none;text-decoration:underline line-through}"
        L"#solid{width:10px;height:10px;border:solid rebeccapurple 4px}</style>"
        L"<body><div id='selector-anchor'></div><div id='selector-target' data-code='Prefix-value' data-tags='new hot' lang='en-US'></div>"
        L"<div id='styled'>decorated</div><div id='solid'></div></body>",&error),
        L"basic CSS syntax fixture parses");
    StyleSheet basicCss;Check(basicCss.Parse(basicCssDoc.StyleText(),&error),L"basic CSS stylesheet parses");
    const auto selectorStyle=basicCss.Compute(basicCssDoc.GetElementById(L"selector-target"));
    Check(selectorStyle.Get(L"width")==L"41px"&&selectorStyle.Get(L"height")==L"22px"&&
          selectorStyle.Get(L"margin-left")==L"3px"&&selectorStyle.Get(L"padding-left")==L"4px"&&
          selectorStyle.Get(L"padding-right")==L"5px"&&selectorStyle.Get(L"background-color")==L"teal",
          L"attribute selector operators, case flags, general siblings and background color extraction work");
    const auto styledNode=basicCssDoc.GetElementById(L"styled");
    const auto styledCss=basicCss.Compute(styledNode);
    Check(styledCss.Get(L"font-style")==L"italic"&&styledCss.Get(L"font-weight")==L"700"&&
          styledCss.Get(L"font-size")==L"12px"&&styledCss.Get(L"line-height")==L"1.5"&&
          styledCss.Get(L"border-top-style")==L"none"&&
          styledCss.Get(L"text-decoration")==L"underline line-through",
          L"font and border shorthands retain basic style components and text decorations");
    Check(StyleSheet::Color(L"#1234")==0x44112233u&&
          StyleSheet::Color(L"#11223380")==0x80112233u&&
          StyleSheet::Color(L"rgb(100% 0% 0% / 50%)")==0x80ff0000u&&
          StyleSheet::Color(L"rebeccapurple")==0xff663399u,
          L"CSS four/eight-digit hex, percentage rgb alpha and common named colors parse");
    LayoutEngine basicCssLayout(basicCssDoc,basicCss);basicCssLayout.Layout(240,120);
    const auto* styledBox=FindLayout(basicCssLayout.Root(),L"styled");
    const auto* solidBox=FindLayout(basicCssLayout.Root(),L"solid");
    Check(styledBox&&solidBox&&std::lround(styledBox->rect.width)==10&&
          std::lround(styledBox->rect.height)==10&&std::lround(solidBox->rect.width)==18&&
          std::lround(solidBox->rect.height)==18,
          L"border-style none suppresses border geometry while a solid border contributes to the box");
    Check(styledBox&&!styledBox->children.empty()&&
          styledBox->children.front()->style.Get(L"text-decoration")==L"underline line-through",
          L"text decorations propagate to the text run that DirectWrite paints");

    Document generatedContentDoc;
    Check(generatedContentDoc.Parse(
        L"<body><div id='empty' aria-label='Edit the rendered document'></div>"
        L"<div id='occupied' aria-label='Hidden label'>text</div></body>",&error),
        L"generated-content fixture parses");
    StyleSheet generatedContentCss;
    Check(generatedContentCss.Parse(
        L"body{margin:0}#empty{height:120px;padding:20px;box-sizing:border-box}"
        L"div:empty::before{content:'[' attr(aria-label) ']'}",&error),
        L"generated-content CSS parses");
    LayoutEngine generatedContentLayout(generatedContentDoc,generatedContentCss);
    generatedContentLayout.Layout(640,240);
    const auto* emptyGenerated=FindLayout(generatedContentLayout.Root(),L"empty");
    const auto* occupiedGenerated=FindLayout(generatedContentLayout.Root(),L"occupied");
    const auto* beforeGenerated=FindPseudo(emptyGenerated,L"before");
    Check(beforeGenerated&&!beforeGenerated->children.empty()&&
          beforeGenerated->children.front()->node->text==L"[Edit the rendered document]"&&
          std::abs(beforeGenerated->rect.y-emptyGenerated->content.y)<0.01f&&
          !FindPseudo(occupiedGenerated,L"before"),
          L":empty generated content resolves attr() at the block's top content edge");

    Document percentageMinHeightDoc;
    Check(percentageMinHeightDoc.Parse(L"<body><main id='fill'></main></body>",&error),
          L"percentage min-height fixture parses");
    StyleSheet percentageMinHeightCss;
    Check(percentageMinHeightCss.Parse(
        L"body{margin:0}#fill{min-height:100%;box-sizing:border-box}",&error),
        L"percentage min-height CSS parses");
    LayoutEngine percentageMinHeightLayout(percentageMinHeightDoc,percentageMinHeightCss);
    percentageMinHeightLayout.Layout(640,720);
    const auto* percentageMinHeightFill=FindLayout(percentageMinHeightLayout.Root(),L"fill");
    Check(percentageMinHeightFill&&std::abs(percentageMinHeightFill->rect.height-720)<0.01f,
          L"percentage min-height resolves against a definite containing block height");

    Document fontDoc;
    Check(fontDoc.Parse(L"<html><head><style>body{font:13px/1.42 'Segoe UI Variable','Segoe UI',sans-serif}button{font:inherit}</style></head><body id='font-body'><button id='font-child'>Text</button></body></html>",&error),
          L"font shorthand fixture parses");
    StyleSheet fontCss;Check(fontCss.Parse(fontDoc.StyleText(),&error),L"font shorthand CSS parses");
    const auto bodyFont=fontCss.Compute(fontDoc.GetElementById(L"font-body"));
    const auto childFont=fontCss.Compute(fontDoc.GetElementById(L"font-child"),&bodyFont);
    Check(bodyFont.Get(L"font-size")==L"13px"&&bodyFont.Get(L"line-height")==L"1.42"&&
          bodyFont.Get(L"font-family").find(L"Segoe UI Variable")!=std::wstring::npos&&
          childFont.Get(L"font-size")==L"13px"&&childFont.Get(L"line-height")==L"1.42"&&
          childFont.Get(L"font-family")==bodyFont.Get(L"font-family")&&
          childFont.Get(L"white-space")==L"nowrap",
          L"font shorthand and font inherit populate the longhands used by text layout");
    Document normalLineDoc;
    Check(normalLineDoc.Parse(
          L"<style>body{margin:0}#normal-line{font-size:13px}</style><div id='normal-line'>Text</div>",
          &error),L"normal line-height fixture parses");
    StyleSheet normalLineCss;Check(normalLineCss.Parse(normalLineDoc.StyleText(),&error),
          L"normal line-height CSS parses");
    LayoutEngine normalLineLayout(normalLineDoc,normalLineCss);normalLineLayout.Layout(200,80);
    const auto* normalLineBox=normalLineLayout.BoxFor(normalLineDoc.GetElementById(L"normal-line"));
    Check(normalLineBox&&std::abs(normalLineBox->rect.height-(13.0f*4.0f/3.0f))<0.001f,
          L"normal line-height follows the browser four-thirds font metric");
    Document inlinePaddingDoc;
    Check(inlinePaddingDoc.Parse(
          L"<style>body{margin:0}.header{display:flex;height:32px;align-items:center}.summary{font-size:11px}.summary code{padding:4px 7px}</style><div class='header'><div id='inline-summary' class='summary'><span>Path</span><code>branch</code></div></div>",
          &error),L"inline padding line-box fixture parses");
    StyleSheet inlinePaddingCss;Check(inlinePaddingCss.Parse(inlinePaddingDoc.StyleText(),&error),
          L"inline padding line-box CSS parses");
    LayoutEngine inlinePaddingLayout(inlinePaddingDoc,inlinePaddingCss);inlinePaddingLayout.Layout(240,80);
    const auto* inlineSummary=inlinePaddingLayout.BoxFor(inlinePaddingDoc.GetElementById(L"inline-summary"));
    Check(inlineSummary&&std::abs(inlineSummary->rect.height-(11.0f*4.0f/3.0f))<0.001f,
          L"vertical padding on a non-atomic inline box does not enlarge its containing line");

    Document rootFontDoc;
    Check(rootFontDoc.Parse(
        L"<html><head><style>:root{font-family:'Segoe UI';font-size:14px}*{box-sizing:border-box}.switch{display:flex;align-items:center;width:220px;height:44px}.copy{display:grid;flex:1;gap:1px;min-width:0}.copy strong{overflow:hidden;font-size:13px;text-overflow:ellipsis;white-space:nowrap}.copy small{font-size:11px}</style></head><body><div class='switch'><span class='copy'><strong id='workspace-title'>Orbit Workspace</strong><small id='workspace-subtitle'>personal</small></span></div><p id='hero-description'>Win32 host description</p></body></html>",&error),
          L"root font inheritance fixture parses");
    StyleSheet rootFontCss;Check(rootFontCss.Parse(rootFontDoc.StyleText(),&error),L"root font inheritance CSS parses");
    LayoutEngine rootFontLayout(rootFontDoc,rootFontCss);rootFontLayout.Layout(320,160);
    const auto* rootBody=rootFontLayout.Root();
    const auto* workspaceTitle=FindLayout(rootFontLayout.Root(),L"workspace-title");
    const auto* workspaceSubtitle=FindLayout(rootFontLayout.Root(),L"workspace-subtitle");
    const auto* heroDescription=FindLayout(rootFontLayout.Root(),L"hero-description");
    Check(rootBody&&heroDescription&&rootBody->style.Get(L"font-size")==L"14px"&&
          heroDescription->style.Get(L"font-size")==L"14px",
          L"a body layout root inherits the author font size from the html :root ancestor");
    Document themedRootDoc;
    Check(themedRootDoc.Parse(
          L"<html data-theme='light'><head><style>:root[data-theme='light']{--theme-ink:#123456}.marker{color:var(--theme-ink)}</style></head><body><span id='theme-marker' class='marker'>x</span></body></html>",&error),
          L"compound root selector fixture parses");
    StyleSheet themedRootCss;Check(themedRootCss.Parse(themedRootDoc.StyleText(),&error),L"compound root selector CSS parses");
    const auto themedHtmlStyle=themedRootCss.Compute(themedRootDoc.QuerySelector(L"html"));
    const auto themedBodyStyle=themedRootCss.Compute(themedRootDoc.Body(),&themedHtmlStyle);
    const auto themedMarkerStyle=themedRootCss.Compute(themedRootDoc.GetElementById(L"theme-marker"),&themedBodyStyle);
    Check(themedMarkerStyle.Get(L"color")==L"#123456",
          L"a compound :root selector applies theme variables through the normal cascade");
    const auto rowsRemainSeparateAtScale=[&](float scale){
        return workspaceTitle&&workspaceSubtitle&&
            std::ceil((workspaceTitle->rect.y+workspaceTitle->rect.height)*scale)<=
            std::floor(workspaceSubtitle->rect.y*scale);
    };
    Check(workspaceTitle&&workspaceSubtitle&&
          workspaceTitle->rect.y+workspaceTitle->rect.height<=workspaceSubtitle->rect.y+0.01f&&
          rowsRemainSeparateAtScale(1.0f)&&rowsRemainSeparateAtScale(1.5f),
          L"overflow-clipped grid labels retain separate intrinsic rows at 100 and 150 percent scaling");

    struct DpiLayoutSample { UINT dpi=0; bool loaded=false; bool clicked=false; std::vector<double> geometry; };
    const auto captureDpiLayout=[&](DPI_AWARENESS_CONTEXT context){
        DpiLayoutSample sample;
        const auto previous=SetThreadDpiAwarenessContext(context);
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
            0,0,2200,1500,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(host){
            sample.dpi=GetDpiForWindow(host);
            constexpr double cssWidth=1328,cssHeight=852;
            const double scale=static_cast<double>(sample.dpi)/96.0;
            RECT bounds{0,0,static_cast<LONG>(std::lround(cssWidth*scale)),
                static_cast<LONG>(std::lround(cssHeight*scale))};
            if(auto view=TWebFrame::View::Create(host,bounds)){
                view->SetMessageHandler([&](const std::wstring& message){
                    sample.clicked=message.find(L"dpi-click")!=std::wstring::npos;
                });
                sample.loaded=view->NavigateToString(
                    L"<style>*{box-sizing:border-box}html,body{width:100%;height:100%;margin:0}.probe{position:absolute;left:10vw;top:10vh;width:50vw;height:25vh;font-size:4vw}</style><button id='dpi-probe' class='probe'>DPI</button><script>document.getElementById('dpi-probe').addEventListener('click',()=>window.chrome.webview.postMessage('dpi-click'));</script>",
                    L"https://dpi.test/");
                std::wstring value,errorText;
                if(sample.loaded&&view->ExecuteScript(
                    L"let r=document.getElementById('dpi-probe').getBoundingClientRect();return [r.x,r.y,r.width,r.height,getComputedStyle(document.getElementById('dpi-probe')).fontSize].join(',');",
                    &value,&errorText)){
                    size_t start=0;
                    while(start<=value.size()){
                        const auto comma=value.find(L',',start);
                        try{sample.geometry.push_back(std::stod(value.substr(start,
                            comma==std::wstring::npos?std::wstring::npos:comma-start)));}
                        catch(...){sample.geometry.clear();break;}
                        if(comma==std::wstring::npos)break;
                        start=comma+1;
                    }
                }
                if(sample.geometry.size()>=4){
                    const int x=static_cast<int>(std::lround(
                        (sample.geometry[0]+sample.geometry[2]/2.0)*scale));
                    const int y=static_cast<int>(std::lround(
                        (sample.geometry[1]+sample.geometry[3]/2.0)*scale));
                    SendMessageW(view->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                    SendMessageW(view->Window(),WM_LBUTTONUP,0,MAKELPARAM(x,y));
                }
            }
            DestroyWindow(host);
        }
        SetThreadDpiAwarenessContext(previous);
        return sample;
    };
    const auto dpi100=captureDpiLayout(DPI_AWARENESS_CONTEXT_UNAWARE);
    const auto dpiMonitor=captureDpiLayout(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto matchesCssViewport=[](const DpiLayoutSample& sample){
        const double expected[]={132.8,85.2,664.0,213.0,53.12};
        if(!sample.loaded||!sample.clicked||sample.geometry.size()!=std::size(expected))return false;
        for(size_t index=0;index<std::size(expected);++index)
            if(std::abs(sample.geometry[index]-expected[index])>0.02)return false;
        return true;
    };
    Check(dpi100.dpi==96&&dpiMonitor.dpi>=96&&matchesCssViewport(dpi100)&&
          matchesCssViewport(dpiMonitor),
          L"96-DPI and per-monitor views preserve CSS geometry and hit testing across physical coordinates");

    Document basicHtmlDoc;
    Check(basicHtmlDoc.Parse(
        L"<body id='basic-body'><p id='first-paragraph'>first<br>line"
        L"<p id='second-paragraph'>second<div id='following-block'>block</div></p>"
        L"<p id='phrasing'><a id='link'><b id='bold'>bold</b> <em id='emphasis'>em</em> <code id='code'>code</code></a>&nbsp;x</body>",
        &error),L"basic HTML element fixture parses");
    const auto basicBody=basicHtmlDoc.GetElementById(L"basic-body");
    const auto firstParagraph=basicHtmlDoc.GetElementById(L"first-paragraph");
    const auto secondParagraph=basicHtmlDoc.GetElementById(L"second-paragraph");
    const auto followingBlock=basicHtmlDoc.GetElementById(L"following-block");
    const auto phrasing=basicHtmlDoc.GetElementById(L"phrasing");
    Check(firstParagraph&&secondParagraph&&followingBlock&&
          firstParagraph->parent.lock()==basicBody&&secondParagraph->parent.lock()==basicBody&&
          followingBlock->parent.lock()==basicBody,
          L"paragraphs close implicitly before another paragraph or block element");
    Check(firstParagraph&&phrasing&&firstParagraph->InnerText()==L"first\nline"&&
          phrasing->InnerText().find(static_cast<wchar_t>(0x00a0))!=std::wstring::npos,
          L"br contributes a line break to inner text and nbsp decodes to a non-breaking space");
    StyleSheet basicHtmlCss;Check(basicHtmlCss.Parse(L"",&error),L"basic HTML default styles parse");
    const auto paragraphStyle=basicHtmlCss.Compute(firstParagraph);
    const auto linkStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"link"));
    const auto boldStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"bold"));
    const auto emphasisStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"emphasis"));
    const auto codeStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"code"));
    Check(paragraphStyle.Is(L"display",L"block")&&paragraphStyle.Get(L"margin")==L"1em 0"&&
          linkStyle.Is(L"display",L"inline")&&boldStyle.Get(L"font-weight")==L"700"&&
          emphasisStyle.Is(L"font-style",L"italic")&&codeStyle.Get(L"font-family")==L"Consolas",
          L"paragraph and common phrasing elements receive browser-like default styles");

    Document breakLayoutDoc;
    Check(breakLayoutDoc.Parse(
        L"<style>body{margin:0;font-size:10px;line-height:20px}p{margin:0}</style>"
        L"<body><p><span id='line-one'>one</span><br><span id='line-two'>two</span><br><br><span id='line-four'>four</span></p><p id='after-lines'>after</p><hr id='rule'></body>",
        &error),L"line break layout fixture parses");
    StyleSheet breakLayoutCss;Check(breakLayoutCss.Parse(breakLayoutDoc.StyleText(),&error),L"line break layout CSS parses");
    LayoutEngine breakLayout(breakLayoutDoc,breakLayoutCss);breakLayout.Layout(240,160);
    const auto* lineOne=FindLayout(breakLayout.Root(),L"line-one");
    const auto* lineTwo=FindLayout(breakLayout.Root(),L"line-two");
    const auto* lineFour=FindLayout(breakLayout.Root(),L"line-four");
    const auto* afterLines=FindLayout(breakLayout.Root(),L"after-lines");
    const auto* rule=FindLayout(breakLayout.Root(),L"rule");
    Check(lineOne&&lineTwo&&lineFour&&afterLines&&
          std::lround(lineTwo->rect.y-lineOne->rect.y)==20&&
          std::lround(lineFour->rect.y-lineTwo->rect.y)==40&&
          std::lround(afterLines->rect.y-lineOne->rect.y)==80,
          L"br creates one line break and consecutive br elements preserve an empty line");
    Check(rule&&std::lround(rule->rect.height)==1&&rule->style.Get(L"background")==L"#808080",
          L"hr receives a visible one-pixel separator default");

    Document preservedWhitespaceDoc;
    Check(preservedWhitespaceDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}body{font-size:10px;line-height:20px}"
        L"#source{display:block;width:160px;white-space:pre-wrap}"
        L"#collapsed{display:block;width:160px;white-space:normal}"
        L"#editor{display:block;width:160px;height:40px;overflow:auto;white-space:pre-wrap}</style>"
        L"<pre id='source'><code id='source-code'></code></pre>"
        L"<div id='collapsed'></div><textarea id='editor'></textarea>",
        &error),L"preserved whitespace fixture parses");
    JavaScriptRuntime preservedWhitespaceJs(preservedWhitespaceDoc);
    Check(preservedWhitespaceJs.Execute(
        L"document.getElementById('source-code').innerHTML='alpha\\nbeta\\ngamma';"
        L"document.getElementById('collapsed').textContent='alpha\\nbeta\\ngamma';"
        L"document.getElementById('editor').value='alpha\\nbeta\\ngamma';",
        nullptr,&error),error.c_str());
    StyleSheet preservedWhitespaceCss;
    Check(preservedWhitespaceCss.Parse(preservedWhitespaceDoc.StyleText(),&error),
          L"preserved whitespace CSS parses");
    LayoutEngine preservedWhitespaceLayout(preservedWhitespaceDoc,preservedWhitespaceCss);
    preservedWhitespaceLayout.Layout(240,180);
    const auto* preservedSource=FindLayout(preservedWhitespaceLayout.Root(),L"source");
    const auto* preservedSourceCode=FindLayout(preservedWhitespaceLayout.Root(),L"source-code");
    const auto* collapsedText=FindLayout(preservedWhitespaceLayout.Root(),L"collapsed");
    const auto* multilineEditor=FindLayout(preservedWhitespaceLayout.Root(),L"editor");
    Check(preservedSourceCode&&collapsedText&&
          preservedSourceCode->rect.height>collapsedText->rect.height*2.5f,
          L"pre-wrap preserves authored line breaks from JavaScript-generated DOM text");
    Check(collapsedText&&collapsedText->rect.height<30,
          L"normal white-space collapses authored line breaks into one line");
    Check(preservedSource&&preservedSourceCode&&collapsedText&&
          preservedSource->rect.height>=preservedSourceCode->rect.height&&
          collapsedText->rect.y>=preservedSourceCode->rect.y+preservedSourceCode->rect.height-0.5f,
          L"an auto-height block contains multiline inline descendants before following flow content");
    Check(multilineEditor&&multilineEditor->scrollHeight>multilineEditor->content.height,
          L"textarea overflow measures every preserved source line");

    Document editableCaretDoc;
    Check(editableCaretDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:320px;min-height:100px;padding:12px;font:16px/24px 'Segoe UI'}p{display:block;margin:0}</style>"
        L"<article id='editable' contenteditable='true'><p id='mixed'>alpha <strong>bravo</strong> charlie</p></article>",
        &error),L"contenteditable caret fixture parses");
    StyleSheet editableCaretCss;
    Check(editableCaretCss.Parse(editableCaretDoc.StyleText(),&error),L"contenteditable caret CSS parses");
    LayoutEngine editableCaretLayout(editableCaretDoc,editableCaretCss);editableCaretLayout.Layout(360,160);
    const auto editableRoot=editableCaretDoc.GetElementById(L"editable");
    const auto mixedParagraph=editableCaretDoc.GetElementById(L"mixed");
    const auto trailingText=mixedParagraph&&!mixedParagraph->children.empty()?mixedParagraph->children.back():std::shared_ptr<Node>{};
    const auto* trailingTextBox=editableCaretLayout.BoxFor(trailingText);
    LayoutRect unpaintedCaret{};
    Check(trailingTextBox&&editableCaretLayout.TextCaretRect(trailingText,2,unpaintedCaret)&&
          unpaintedCaret.x>trailingTextBox->rect.x,
          L"contenteditable caret geometry resolves a DOM text offset before the first paint");
    std::shared_ptr<Node> hitText;size_t hitTextOffset=0;
    Check(trailingTextBox&&editableCaretLayout.HitTestText(mixedParagraph,
              trailingTextBox->rect.x+trailingTextBox->rect.width*0.5f,
              trailingTextBox->rect.y+trailingTextBox->rect.height*0.5f,hitText,hitTextOffset)&&
          hitText==trailingText&&hitTextOffset>0&&hitTextOffset<trailingText->text.size(),
          L"contenteditable pointer placement selects the nearest DOM text offset before the first paint");

    Document emptyEditableDoc;
    Check(emptyEditableDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:240px;min-height:80px;padding:10px;font-size:16px;line-height:24px}article:empty::before{content:attr(aria-label)}</style>"
        L"<article id='empty-editor' contenteditable='true' aria-label='Edit here'></article>",
        &error),L"empty contenteditable fixture parses");
    const auto emptyEditable=emptyEditableDoc.GetElementById(L"empty-editor");
    auto emptyTextNode=std::make_shared<Node>();emptyTextNode->type=NodeType::Text;
    emptyTextNode->tag=L"#text";emptyTextNode->parent=emptyEditable;emptyEditable->children.push_back(emptyTextNode);
    emptyEditableDoc.Reindex();
    StyleSheet emptyEditableCss;
    Check(emptyEditableCss.Parse(emptyEditableDoc.StyleText(),&error),L"empty contenteditable CSS parses");
    LayoutEngine emptyEditableLayout(emptyEditableDoc,emptyEditableCss);emptyEditableLayout.Layout(280,140);
    LayoutRect emptyCaret{};const auto* emptyEditorBox=emptyEditableLayout.BoxFor(emptyEditable);
    Check(emptyEditorBox&&emptyEditableLayout.BoxFor(emptyTextNode)==nullptr&&
          emptyEditableLayout.TextCaretRect(emptyTextNode,0,emptyCaret)&&
          std::abs(emptyCaret.x-emptyEditorBox->content.x)<0.01f&&
          std::abs(emptyCaret.y-emptyEditorBox->content.y)<0.01f&&
          std::lround(emptyCaret.height)==24,
          L"an empty contenteditable paints a line-height caret at its content origin even while its placeholder pseudo-element is rendered");

    Document mediaDoc;
    Check(mediaDoc.Parse(L"<style>.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}@media (max-width:1180px){.grid{grid-template-columns:1fr}}</style><div class='grid'></div>",&error),
          L"media query fixture parses");
    StyleSheet mediaCss;Check(mediaCss.Parse(mediaDoc.StyleText(),&error),L"media query CSS parses");
    mediaCss.SetViewport(1320,700);Check(mediaCss.Compute(mediaDoc.QuerySelector(L".grid")).Get(L"grid-template-columns")==L"repeat(2,minmax(0,1fr))",
          L"max-width media rule stays inactive above its breakpoint");
    const auto wideMediaVersion=mediaCss.Version();mediaCss.SetViewport(1280,700);
    Check(mediaCss.Version()==wideMediaVersion,L"viewport changes within one media-query range preserve computed-style caches");
    mediaCss.SetViewport(1000,700);Check(mediaCss.Compute(mediaDoc.QuerySelector(L".grid")).Get(L"grid-template-columns")==L"1fr",
          L"max-width media rule participates in the normal cascade below its breakpoint");
    Check(mediaCss.Version()!=wideMediaVersion,L"crossing a media-query breakpoint invalidates computed-style caches");

    Document relayoutDoc;
    Check(relayoutDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.panel{width:50vw;height:20px}@media (max-width:700px){.panel{width:25vw}}</style><div id='relayout-panel' class='panel'></div>",&error),
          L"viewport relayout fixture parses");
    StyleSheet relayoutCss;Check(relayoutCss.Parse(relayoutDoc.StyleText(),&error),L"viewport relayout CSS parses");
    LayoutEngine relayoutEngine(relayoutDoc,relayoutCss);relayoutEngine.Layout(1000,100);
    const auto* relayoutRoot=relayoutEngine.Root();const auto* initialRelayoutPanel=FindLayout(relayoutRoot,L"relayout-panel");
    relayoutEngine.Relayout(800,100);const auto* resizedRelayoutPanel=FindLayout(relayoutEngine.Root(),L"relayout-panel");
    Check(relayoutEngine.Root()==relayoutRoot&&initialRelayoutPanel==resizedRelayoutPanel&&
          resizedRelayoutPanel&&std::lround(resizedRelayoutPanel->rect.width)==400,
          L"viewport-only relayout reuses layout and text caches while resolving viewport units");
    relayoutEngine.Relayout(600,100);const auto* narrowRelayoutPanel=FindLayout(relayoutEngine.Root(),L"relayout-panel");
    Check(narrowRelayoutPanel&&std::lround(narrowRelayoutPanel->rect.width)==150,
          L"viewport relayout falls back to a full style rebuild at media-query breakpoints");

    Document viewportFontDoc;
    Check(viewportFontDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.title{font-size:clamp(20px,4vw,60px)}</style><div id='viewport-title' class='title'>Title</div>",&error),
          L"viewport font-size fixture parses");
    StyleSheet viewportFontCss;Check(viewportFontCss.Parse(viewportFontDoc.StyleText(),&error),L"viewport font-size CSS parses");
    LayoutEngine viewportFontLayout(viewportFontDoc,viewportFontCss);viewportFontLayout.Layout(1000,100);
    const auto* wideViewportTitle=FindLayout(viewportFontLayout.Root(),L"viewport-title");
    Check(wideViewportTitle&&std::abs(StyleSheet::Length(wideViewportTitle->style.Get(L"font-size"),16,16)-40)<0.01f,
          L"viewport-relative font size resolves against the CSS viewport");
    viewportFontLayout.Relayout(500,100);
    const auto* narrowViewportTitle=FindLayout(viewportFontLayout.Root(),L"viewport-title");
    Check(narrowViewportTitle&&std::abs(StyleSheet::Length(narrowViewportTitle->style.Get(L"font-size"),16,16)-20)<0.01f,
          L"viewport-relative font size recomputes after a DPI-independent viewport change");

    Document gridDoc;
    Check(gridDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.shell{width:700px;height:400px;display:grid;grid-template-rows:52px minmax(0,1fr) 28px}.named{width:1000px;height:80px;display:grid;grid-template-columns:minmax(100px,140px) minmax(100px,140px) minmax(180px,1fr) max-content minmax(180px,1fr);grid-template-areas:'first second fluid intrinsic last';column-gap:10px}.first{grid-area:first}.second{grid-area:second}.fluid{grid-area:fluid}.intrinsic{grid-area:intrinsic;width:70px}.last{grid-area:last}</style><div class='shell'><div id='grid-top'></div><div id='grid-main'></div><div id='grid-foot'></div></div><div class='named'><div id='grid-last' class='last'></div><div id='grid-first' class='first'></div><div id='grid-intrinsic' class='intrinsic'></div><div id='grid-fluid' class='fluid'></div><div id='grid-second' class='second'></div></div>",&error),
          L"explicit and named grid fixture parses");
    StyleSheet gridCss;Check(gridCss.Parse(gridDoc.StyleText(),&error),L"explicit and named grid CSS parses");
    LayoutEngine gridLayout(gridDoc,gridCss);gridLayout.Layout(1000,480);
    const auto* gridTop=FindLayout(gridLayout.Root(),L"grid-top");
    const auto* gridMain=FindLayout(gridLayout.Root(),L"grid-main");
    const auto* gridFoot=FindLayout(gridLayout.Root(),L"grid-foot");
    Check(gridTop&&gridMain&&gridFoot&&std::lround(gridTop->rect.height)==52&&
          std::lround(gridMain->rect.height)==320&&std::lround(gridMain->rect.y)==52&&
          std::lround(gridFoot->rect.height)==28&&std::lround(gridFoot->rect.y)==372,
          L"grid-template-rows preserves fixed tracks and assigns the remainder to fr");
    const auto* gridFirst=FindLayout(gridLayout.Root(),L"grid-first");
    const auto* gridSecond=FindLayout(gridLayout.Root(),L"grid-second");
    const auto* gridFluid=FindLayout(gridLayout.Root(),L"grid-fluid");
    const auto* gridIntrinsic=FindLayout(gridLayout.Root(),L"grid-intrinsic");
    const auto* gridLast=FindLayout(gridLayout.Root(),L"grid-last");
    Check(gridFirst&&gridSecond&&gridFluid&&gridIntrinsic&&gridLast&&
          gridFirst->rect.x<gridSecond->rect.x&&gridSecond->rect.x<gridFluid->rect.x&&
          gridFluid->rect.x<gridIntrinsic->rect.x&&gridIntrinsic->rect.x<gridLast->rect.x&&
          std::lround(gridFirst->rect.width)==140&&std::lround(gridSecond->rect.width)==140&&
          std::lround(gridIntrinsic->rect.width)==70,
          L"grid-template-areas places reordered children and resolves capped/content/fr tracks");

    Document gridButtonDoc;
    Check(gridButtonDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}button{font:inherit}.list{display:grid;width:620px;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.item{display:grid;grid-template-columns:30px 1fr;padding:10px 11px;border:1px solid transparent}.copy strong,.copy small{display:block}.copy strong{font-size:12px}.copy small{margin-top:3px;font-size:10px}</style>"
          L"<div class='list'><button id='grid-button' class='item'><span>icon</span><span class='copy'><strong id='grid-button-title'>name</strong><small id='grid-button-detail'>path</small></span></button></div>",&error),
          L"grid button intrinsic-size fixture parses");
    StyleSheet gridButtonCss;Check(gridButtonCss.Parse(gridButtonDoc.StyleText(),&error),L"grid button intrinsic-size CSS parses");
    LayoutEngine gridButtonLayout(gridButtonDoc,gridButtonCss);gridButtonLayout.Layout(700,200);
    const auto* gridButton=FindLayout(gridButtonLayout.Root(),L"grid-button");
    const auto* gridButtonTitle=FindLayout(gridButtonLayout.Root(),L"grid-button-title");
    const auto* gridButtonDetail=FindLayout(gridButtonLayout.Root(),L"grid-button-detail");
    Check(gridButton&&gridButtonTitle&&gridButtonDetail&&std::lround(gridButton->rect.height)==54&&
          gridButtonTitle->rect.y+gridButtonTitle->rect.height<=gridButtonDetail->rect.y+0.01f,
          L"grid buttons size from blockified inline items and preserve their vertical content flow");

    Document intrinsicDoc;
    Check(intrinsicDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.card{width:300px;display:grid;padding:10px;border:1px solid black}.field{display:grid;gap:5px}.label{height:16px}.control{position:relative}.editor{display:block;width:100%;height:32px}.floating{position:absolute;right:0;top:50%;height:12px}.hint{height:12px}</style><div id='intrinsic-card' class='card'><div class='field'><span class='label'>Label</span><div class='control'><input class='editor'><span class='floating'>v</span></div><span class='hint'>Hint</span></div></div>",&error),
          L"intrinsic positioned-child fixture parses");
    StyleSheet intrinsicCss;Check(intrinsicCss.Parse(intrinsicDoc.StyleText(),&error),L"intrinsic positioned-child CSS parses");
    LayoutEngine intrinsicLayout(intrinsicDoc,intrinsicCss);intrinsicLayout.Layout(400,200);
    const auto* intrinsicCard=FindLayout(intrinsicLayout.Root(),L"intrinsic-card");
    const auto* intrinsicEditor=intrinsicLayout.BoxFor(intrinsicDoc.QuerySelector(L".editor"));
    Check(intrinsicCard&&intrinsicEditor&&std::lround(intrinsicCard->rect.height)==92&&
          std::lround(intrinsicEditor->rect.width)==278,
          L"absolute descendants do not increase intrinsic height and percentage controls resolve against their containing block");

    Document absolutePaddingDoc;
    Check(absolutePaddingDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.host{position:relative;width:100px;height:60px;padding:10px 15px;border:2px solid black}.flex{display:flex}.grid{display:grid}.fill{position:absolute;inset:0}</style>"
          L"<div class='host flex'><span id='absolute-flex' class='fill'></span></div><div class='host grid'><span id='absolute-grid' class='fill'></span></div><div class='host'><span id='absolute-block' class='fill'></span></div>",&error),
          L"absolute padding-box fixture parses");
    StyleSheet absolutePaddingCss;Check(absolutePaddingCss.Parse(absolutePaddingDoc.StyleText(),&error),L"absolute padding-box CSS parses");
    LayoutEngine absolutePaddingLayout(absolutePaddingDoc,absolutePaddingCss);absolutePaddingLayout.Layout(400,240);
    const auto* absoluteFlex=FindLayout(absolutePaddingLayout.Root(),L"absolute-flex");
    const auto* absoluteGrid=FindLayout(absolutePaddingLayout.Root(),L"absolute-grid");
    const auto* absoluteBlock=FindLayout(absolutePaddingLayout.Root(),L"absolute-block");
    Check(absoluteFlex&&absoluteGrid&&absoluteBlock&&
          std::lround(absoluteFlex->rect.x)==2&&std::lround(absoluteFlex->rect.width)==96&&
          std::lround(absoluteGrid->rect.x)==2&&std::lround(absoluteGrid->rect.width)==96&&
          std::lround(absoluteBlock->rect.x)==2&&std::lround(absoluteBlock->rect.width)==96,
          L"absolute insets resolve against the padding box in flex, grid, and block containers");

    Document selectDoc;
    Check(selectDoc.Parse(L"<html><head><style>:root{color-scheme:light}*{box-sizing:border-box;margin:0;padding:0}select{width:140px;height:32px;background:#f0f4ff;color:#182131}</style></head><body><select id='choice' value='b'><option value='a'>Alpha</option><option value='b'>Beta</option><option value='c'>Gamma</option></select><datalist id='suggestions'><option value='one'></option></datalist></body></html>",&error),
          L"select control fixture parses");
    StyleSheet selectCss;Check(selectCss.Parse(selectDoc.StyleText(),&error),L"select control CSS parses");
    LayoutEngine selectLayout(selectDoc,selectCss);selectLayout.Layout(300,100);
    const auto* selectBox=FindLayout(selectLayout.Root(),L"choice");
    const auto* dataListBox=FindLayout(selectLayout.Root(),L"suggestions");
    Check(selectBox&&std::lround(selectBox->rect.width)==140&&std::lround(selectBox->rect.height)==32&&
          selectBox->style.Get(L"color-scheme")==L"light"&&dataListBox&&!dataListBox->visible,
          L"select and datalist are atomic controls instead of laying out their option children");
    const auto selectPalette=selectBox?ResolveSelectPopupPalette(selectBox->style,
        StyleSheet::Color(selectBox->style.Get(L"background-color"),0)):SelectPopupPalette{};
    Check(selectPalette.background==0xfff0f4ff&&selectPalette.border==0xffd8d8d8&&
          selectPalette.selectedBackground==0xff767676&&selectPalette.color==0xff182131&&
          selectPalette.selectedColor==0xffffffff,
          L"select popup inherits its computed control surface and common UA selection colors");
    ComputedStyle darkSelectStyle;if(selectBox)*darkSelectStyle.values=*selectBox->style.values;
    (*darkSelectStyle.values)[L"color-scheme"]=L"dark";
    const auto darkSelectPalette=ResolveSelectPopupPalette(darkSelectStyle,0xff242424);
    Check(darkSelectPalette.background==0xff242424&&darkSelectPalette.border==0xffa8a8a8&&
          darkSelectPalette.selectedBackground==0xffc6c6c6&&darkSelectPalette.selectedColor==0xff1b1b1b,
          L"select popup defaults follow the inherited dark color scheme without per-control CSS");

    Document percentGridDoc;
    Check(percentGridDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.cell{width:140px;display:grid}.control{width:100%;height:32px}</style><div class='cell'><select id='percent-grid-control' class='control'><option>Choice</option></select></div>",&error),
          L"percentage grid-control fixture parses");
    StyleSheet percentGridCss;Check(percentGridCss.Parse(percentGridDoc.StyleText(),&error),L"percentage grid-control CSS parses");
    LayoutEngine percentGridLayout(percentGridDoc,percentGridCss);percentGridLayout.Layout(300,100);
    const auto* percentGridControl=FindLayout(percentGridLayout.Root(),L"percent-grid-control");
    Check(percentGridControl&&std::lround(percentGridControl->rect.width)==140,
          L"percentage-sized grid controls resolve against the track instead of growing it to their fallback intrinsic width");

    HWND selectHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
        -10000,-10000,300,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(selectHost!=nullptr,L"select interaction host is created");
    if(selectHost){
        RECT selectBounds{0,0,300,100};auto selectView=TWebFrame::View::Create(selectHost,selectBounds);
        Check(selectView!=nullptr,L"select interaction TWebFrame view is created");
        if(selectView){
            std::vector<std::wstring> selectEvents;
            selectView->SetMessageHandler([&](const std::wstring& message){selectEvents.push_back(message);});
            auto clickControlCenter=[&](const wchar_t* id){
                std::wstring center,centerError;
                const auto script=L"const r=document.getElementById('"+std::wstring(id)+
                    L"').getBoundingClientRect();return Math.round(r.x+r.width/2)+','+"
                    L"Math.round(r.y+r.height/2);";
                Check(selectView->ExecuteScript(script,&center,&centerError),centerError.c_str());
                const auto comma=center.find(L',');
                if(comma==std::wstring::npos)return;
                SendMessageW(selectView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,
                    MAKELPARAM(std::stoi(center.substr(0,comma)),std::stoi(center.substr(comma+1))));
            };
            Check(selectView->NavigateToString(
                L"<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%}select{width:140px;height:32px;background:#f0f4ff;color:#182131}</style>"
                L"<select id='native-choice' value='a'><option value='a'>Alpha</option><option value='b'>Beta</option><option value='c'>Gamma</option></select>"
                L"<script>const choice=document.getElementById('native-choice');choice.addEventListener('input',()=>{window.chrome.webview.postMessage('input:'+choice.value);});choice.addEventListener('change',()=>{window.chrome.webview.postMessage('change:'+choice.value);});</script>"),
                selectView->LastError().c_str());
            clickControlCenter(L"native-choice");
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_DOWN,0);
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_RETURN,0);
            std::wstring selectionResult,selectionError;
            Check(selectView->ExecuteScript(
                L"return document.getElementById('native-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(selectionResult==L"b"&&selectEvents.size()==2&&
                  selectEvents[0]==L"input:b"&&selectEvents[1]==L"change:b",
                  L"select keyboard selection updates value and dispatches input and change events");

            selectEvents.clear();
            Check(selectView->NavigateToString(
                L"<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%}body{padding-top:36px}input{width:180px;height:32px;background:#f0f4ff;color:#182131}</style>"
                L"<input id='editable-choice' list='editable-options' placeholder='Choose or type'><datalist id='editable-options'>"
                L"<option value='10M'></option><option value='100M'></option><option value='650M' label='CD'></option></datalist>"
                L"<script>const choice=document.getElementById('editable-choice');choice.addEventListener('input',()=>{window.chrome.webview.postMessage('input:'+choice.value);});choice.addEventListener('change',()=>{window.chrome.webview.postMessage('change:'+choice.value);});</script>"),
                selectView->LastError().c_str());
            clickControlCenter(L"editable-choice");
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_DOWN,0);
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_RETURN,0);
            selectionResult.clear();selectionError.clear();
            Check(selectView->ExecuteScript(
                L"return document.getElementById('editable-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(GetWindow(selectView->Window(),GW_CHILD)==nullptr,
                  L"input list uses the TWebFrame input path without a child edit control");
            Check(selectionResult==L"10M",(L"input list commits the chosen datalist option; actual value: "+selectionResult).c_str());
            Check(selectEvents.size()==2&&selectEvents[0]==L"input:10M"&&selectEvents[1]==L"change:10M",
                  L"input list selection dispatches one input event and one change event");
            selectEvents.clear();SendMessageW(selectView->Window(),WM_CHAR,L'G',0);
            selectionResult.clear();selectionError.clear();
            Check(selectView->ExecuteScript(
                L"return document.getElementById('editable-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(selectionResult==L"10MG"&&selectEvents.size()==1&&selectEvents[0]==L"input:10MG",
                  L"input list remains directly editable after choosing a suggestion");
            const auto editableAccessibility=selectView->DumpAccessibilityJson();
            Check(editableAccessibility.find(L"\"automationId\":\"editable-choice\"")!=std::wstring::npos&&
                  editableAccessibility.find(L"\"controlType\":"+std::to_wstring(UIA_ComboBoxControlTypeId))!=std::wstring::npos,
                  L"input list exposes editable-combobox accessibility semantics");
        }
        selectView.reset();DestroyWindow(selectHost);
    }

    HWND accessibilityHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
        -10000,-10000,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(accessibilityHost!=nullptr,L"accessibility interaction host is created");
    if(accessibilityHost){
        RECT bounds{0,0,640,360};auto view=TWebFrame::View::Create(accessibilityHost,bounds);
        Check(view!=nullptr,L"accessibility TWebFrame view is created");
        if(view){
            Check(view->NavigateToString(
                L"<style>*{box-sizing:border-box}body{margin:0}button,input,select,[role=menuitem]{display:block;width:180px;height:28px}.menu-closed{display:none}</style>"
                L"<button id='plain'>Plain</button><button id='tab-two' tabindex='2'>Two</button>"
                L"<button id='tab-one' tabindex='1' data-automation-id='primary-action' aria-label='Primary action' aria-expanded='false'>One</button>"
                L"<input id='value-field' aria-label='Value field' value='start'><input id='toggle-field' type='checkbox' aria-label='Toggle field'>"
                L"<button id='disabled-action' disabled aria-label='Disabled action'>Disabled</button>"
                L"<select id='selection' aria-label='Selection'><option id='option-a' value='a'>Alpha</option><option id='option-b' value='b'>Beta</option></select>"
                L"<div><button id='menu-trigger' aria-controls='command-menu'>Commands</button><div id='command-menu' class='menu-closed' role='menu' aria-label='Commands'><button role='menuitem' id='menu-one'>Menu one</button><button role='menuitem' id='menu-two'>Menu two</button></div></div>"
                L"<main id='content-region'><section><span>Section text</span></section></main>"
                L"<div id='live' role='status' aria-live='polite'>Ready</div>"
                L"<script>let accessibilityLog=[];document.addEventListener('focusin',(e)=>accessibilityLog.push('in:'+e.target.id));document.addEventListener('focusout',(e)=>accessibilityLog.push('out:'+e.target.id));document.addEventListener('click',(e)=>accessibilityLog.push('click:'+e.target.id));document.getElementById('menu-trigger').addEventListener('click',()=>document.getElementById('command-menu').classList.remove('menu-closed'));</script>"),
                view->LastError().c_str());
            std::wstring result,scriptError;
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-one",
                  L"Tab follows positive tabindex ordering and marks keyboard focus visible");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-two",
                  L"Tab advances through equal focus groups in document order");
            BYTE oldKeyboard[256]{},shiftKeyboard[256]{};GetKeyboardState(oldKeyboard);std::copy(std::begin(oldKeyboard),std::end(oldKeyboard),std::begin(shiftKeyboard));shiftKeyboard[VK_SHIFT]=0x80;SetKeyboardState(shiftKeyboard);
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);SetKeyboardState(oldKeyboard);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-one",
                  L"Shift+Tab reverses sequential focus order");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_RETURN,0);
            Check(view->ExecuteScript(L"return accessibilityLog.join(',');",&result,&scriptError)&&
                  result.find(L"in:tab-one")!=std::wstring::npos&&result.find(L"out:tab-one")!=std::wstring::npos&&
                  result.find(L"click:tab-one")!=std::wstring::npos,
                  L"focusin/focusout bubble and Enter activates a focused button");
            Check(view->ExecuteScript(L"document.getElementById('menu-trigger').focus();",nullptr,&scriptError),scriptError.c_str());
            SendMessageW(view->Window(),WM_KEYDOWN,VK_DOWN,0);
            Check(view->ExecuteScript(L"return document.activeElement.id;",&result,&scriptError)&&result==L"menu-one",
                  L"ArrowDown opens an associated menu and focuses its first item");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_DOWN,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"menu-two",
                  L"menu arrow navigation moves focus among menuitems");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_SPACE,0);
            Check(view->ExecuteScript(L"return accessibilityLog[accessibilityLog.length-1];",&result,&scriptError)&&result==L"click:menu-two",
                  L"Space activates a focused menuitem");

            const auto accessibilityJson=view->DumpAccessibilityJson();
            Check(accessibilityJson.find(L"\"automationId\":\"primary-action\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"automationId\":\"value-field\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"automationId\":\"option-b\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"name\":\"Primary action\"")!=std::wstring::npos,
                  L"accessibility diagnostics expose stable explicit automation ids and accessible names");
            Check(accessibilityJson.find(L"\"automationId\":\"content-region\",\"name\":\"\"")!=std::wstring::npos,
                  L"structural accessibility containers do not duplicate all descendant text in their names");

            using Microsoft::WRL::ComPtr;
            ComPtr<IUIAutomation> automation;
            HRESULT automationResult=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,
                                                       IID_PPV_ARGS(automation.ReleaseAndGetAddressOf()));
            Check(SUCCEEDED(automationResult),L"UI Automation client is available");
            if(SUCCEEDED(automationResult)){
                ComPtr<IUIAutomationElement> root;
                automationResult=automation->ElementFromHandle(view->Window(),root.ReleaseAndGetAddressOf());
                Check(SUCCEEDED(automationResult)&&root,L"WM_GETOBJECT returns the TWebFrame UI Automation fragment root");
                auto findById=[&](const wchar_t* id){
                    ComPtr<IUIAutomationElement> element;if(!root)return element;
                    VARIANT value{};value.vt=VT_BSTR;value.bstrVal=SysAllocString(id);
                    ComPtr<IUIAutomationCondition> condition;
                    if(SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,value,condition.ReleaseAndGetAddressOf())))
                        root->FindFirst(TreeScope_Descendants,condition.Get(),element.ReleaseAndGetAddressOf());
                    VariantClear(&value);return element;
                };
                auto primary=findById(L"primary-action");Check(primary!=nullptr,L"UIA tree finds an element by stable AutomationId");
                if(primary){
                    BSTR name=nullptr;primary->get_CurrentName(&name);Check(name&&std::wstring(name)==L"Primary action",L"UIA exposes aria-label as Name");SysFreeString(name);
                    RECT rectangle{};Check(SUCCEEDED(primary->get_CurrentBoundingRectangle(&rectangle))&&rectangle.right>rectangle.left&&rectangle.bottom>rectangle.top,
                        L"UIA fragment exposes non-empty screen bounds");
                    ComPtr<IUIAutomationInvokePattern> invokePattern;
                    Check(SUCCEEDED(primary->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(invokePattern.ReleaseAndGetAddressOf())))&&invokePattern,
                        L"button exposes the Invoke pattern");
                    if(invokePattern)invokePattern->Invoke();
                    ComPtr<IUIAutomationExpandCollapsePattern> expandPattern;
                    Check(SUCCEEDED(primary->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,IID_PPV_ARGS(expandPattern.ReleaseAndGetAddressOf())))&&expandPattern,
                        L"aria-expanded exposes the ExpandCollapse pattern");
                    if(expandPattern){expandPattern->Expand();ExpandCollapseState state=ExpandCollapseState_Collapsed;expandPattern->get_CurrentExpandCollapseState(&state);Check(state==ExpandCollapseState_Expanded,L"expanded state is exposed through UIA");}
                }
                auto valueField=findById(L"value-field");
                if(valueField){ComPtr<IUIAutomationValuePattern> valuePattern;Check(SUCCEEDED(valueField->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(valuePattern.ReleaseAndGetAddressOf())))&&valuePattern,L"text input exposes the Value pattern");if(valuePattern){BSTR updated=SysAllocString(L"updated");valuePattern->SetValue(updated);SysFreeString(updated);}}
                auto toggleField=findById(L"toggle-field");
                if(toggleField){ComPtr<IUIAutomationTogglePattern> togglePattern;Check(SUCCEEDED(toggleField->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(togglePattern.ReleaseAndGetAddressOf())))&&togglePattern,L"checkbox exposes the Toggle pattern");if(togglePattern){togglePattern->Toggle();ToggleState state=ToggleState_Off;togglePattern->get_CurrentToggleState(&state);Check(state==ToggleState_On,L"checked state is exposed through UIA");}}
                auto optionB=findById(L"option-b");Check(optionB!=nullptr,L"UIA tree exposes native select options");
                if(optionB){ComPtr<IUIAutomationSelectionItemPattern> selectionItem;Check(SUCCEEDED(optionB->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(selectionItem.ReleaseAndGetAddressOf())))&&selectionItem,L"option exposes the SelectionItem pattern");if(selectionItem)selectionItem->Select();}
                const bool actionsUpdated=view->ExecuteScript(L"return document.getElementById('value-field').value+'|'+document.getElementById('toggle-field').checked+'|'+document.getElementById('selection').value+'|'+document.getElementById('tab-one').getAttribute('aria-expanded');",&result,&scriptError)&&result==L"updated|true|b|true";
                if(!actionsUpdated)std::wcerr<<L"UIA action state: "<<result<<L"\n";
                Check(actionsUpdated,
                      L"UIA Invoke/Value/Toggle/Selection/ExpandCollapse actions update the DOM");
                auto disabled=findById(L"disabled-action");if(disabled){BOOL enabled=TRUE;disabled->get_CurrentIsEnabled(&enabled);Check(!enabled,L"disabled state is exposed through UIA");}
                auto menuItem=findById(L"menu-one");if(menuItem){VARIANT roleValue{};menuItem->GetCurrentPropertyValue(UIA_AriaRolePropertyId,&roleValue);Check(roleValue.vt==VT_BSTR&&std::wstring(roleValue.bstrVal)==L"menuitem",L"HTML role is exposed through UIA");VariantClear(&roleValue);}
                auto live=findById(L"live");if(live){VARIANT liveValue{};live->GetCurrentPropertyValue(UIA_LiveSettingPropertyId,&liveValue);Check(liveValue.vt==VT_I4&&liveValue.lVal==Polite,L"aria-live politeness is exposed through UIA");VariantClear(&liveValue);}
                auto selection=findById(L"selection");if(selection){ComPtr<IUIAutomationSelectionPattern> selectionPattern;Check(SUCCEEDED(selection->GetCurrentPatternAs(UIA_SelectionPatternId,IID_PPV_ARGS(selectionPattern.ReleaseAndGetAddressOf())))&&selectionPattern,L"select exposes the Selection pattern");if(selectionPattern){ComPtr<IUIAutomationElementArray> selected;selectionPattern->GetCurrentSelection(selected.ReleaseAndGetAddressOf());int length=0;if(selected)selected->get_Length(&length);Check(length==1,L"Selection pattern returns the selected option");}}
            }
        }
        view.reset();DestroyWindow(accessibilityHost);
    }

    const wchar_t resizeHostClass[]=L"TWebFrame.Tests.ResizeHost";
    WNDCLASSW resizeClass{};resizeClass.lpfnWndProc=ResizeHostWindowProc;
    resizeClass.hInstance=GetModuleHandleW(nullptr);resizeClass.lpszClassName=resizeHostClass;
    RegisterClassW(&resizeClass);
    HWND resizeHost=CreateWindowExW(WS_EX_TOOLWINDOW,resizeHostClass,L"",WS_POPUP|WS_THICKFRAME,
        -10000,-10000,400,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(resizeHost!=nullptr,L"resize forwarding host is created");
    if(resizeHost){
        RECT bounds{0,0,400,240};auto resizeView=TWebFrame::View::Create(resizeHost,bounds);
        Check(resizeView!=nullptr,L"resize forwarding TWebFrame view is created");
        if(resizeView){
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0}.surface{width:100vw;height:100vh;background:#183153;color:white}</style><div class='surface'>Buffered resize</div>"),
                L"back-buffer paint fixture loads");
            for(int width=400;width<=720;width+=16){
                RECT frameBounds{0,0,width,240+(width-400)/4};resizeView->SetBounds(frameBounds);
                RedrawWindow(resizeView->Window(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
            }
            Check(IsWindow(resizeView->Window()),L"off-screen back-buffer painting survives repeated live-size frames");
            const LRESULT resizeHits[]={HTLEFT,HTRIGHT,HTTOP,HTBOTTOM,HTTOPLEFT,HTTOPRIGHT,HTBOTTOMLEFT,HTBOTTOMRIGHT};
            bool allForwarded=true;
            for(const auto hit:resizeHits){
                resizeHostHit=hit;resizeHostButtonDown=0;
                SendMessageW(resizeView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                allForwarded=allForwarded&&resizeHostButtonDown==static_cast<WPARAM>(hit);
            }
            Check(allForwarded,L"TWebFrame forwards all four edges and four corner resize hits to its parent window");
            resizeHostHit=HTRIGHT;SendMessageW(resizeView->Window(),WM_SETCURSOR,
                reinterpret_cast<WPARAM>(resizeView->Window()),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
            Check(GetCursor()==LoadCursorW(nullptr,IDC_SIZEWE),L"parent resize border selects the matching system sizing cursor");
            resizeHostHit=HTCLIENT;
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0;padding:0}.row{display:flex;width:200px;height:40px;background:#111}.row:hover{width:220px;background:#345}.row span{display:block;width:100px;height:40px}</style>"
                L"<button id='hover-row' class='row'><span id='hover-first'><strong>Workspace</strong></span><span id='hover-second'><small>Recent</small></span></button>"),
                L"ancestor hover fixture loads");
            std::wstring hoverResult,hoverError;
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,MAKELPARAM(20,20));
            Check(resizeView->ExecuteScript(
                L"let row=document.querySelector('#hover-row:hover');return row.id+'|'+document.querySelector('#hover-first:hover').id+'|'+String(row.getBoundingClientRect().width);",
                &hoverResult,&hoverError)&&hoverResult==L"hover-row|hover-first|220",
                L"hover applies its style to a nested hit target and all of its ancestors");
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,MAKELPARAM(120,20));
            Check(resizeView->ExecuteScript(
                L"return document.querySelector('#hover-row:hover').id+'|'+document.querySelector('#hover-second:hover').id;",
                &hoverResult,&hoverError)&&hoverResult==L"hover-row|hover-second",
                L"moving between child elements preserves the parent hover state");
            SendMessageW(resizeView->Window(),WM_MOUSELEAVE,0,0);
            Check(resizeView->ExecuteScript(
                L"return document.querySelector('#hover-row:hover')===null&&document.getElementById('hover-row').getBoundingClientRect().width===200;",
                &hoverResult,&hoverError)&&hoverResult==L"true",
                L"leaving the view clears hover styles from the complete ancestor path");
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0}body{background:#1b241e}.field{display:flex;width:200px;height:34px;background:rgba(0,0,0,.13)}.field:focus-within{background:rgba(0,0,0,.2)}.field input{width:100%;height:100%;border:0;background:transparent;color:#e9ecea}</style><body><label class='field'><input type='search' placeholder='Search'></label></body>"),
                L"transparent custom editor fixture loads");
            SendMessageW(resizeView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(50,17));
            Check(GetWindow(resizeView->Window(),GW_CHILD)==nullptr,
                  L"focused text input does not create a native child editor");
            SendMessageW(resizeView->Window(),WM_CHAR,L'x',1);
            Check(resizeView->ExecuteScript(L"return document.querySelector('input').value;",
                                            &hoverResult,&hoverError)&&hoverResult==L"x",
                  L"the first custom-input character replaces the visible placeholder with DOM text");
        }
        resizeView.reset();DestroyWindow(resizeHost);
    }
    UnregisterClassW(resizeHostClass,GetModuleHandleW(nullptr));

    Document positionedDoc;
    Check(positionedDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.overlay{position:fixed;inset:0;display:grid;place-items:center;opacity:0}.dialog{width:40px;height:20px;transform:translateY(-50%)}</style><div id='overlay' class='overlay'><div id='dialog' class='dialog'></div></div>",&error),
          L"positioned layout fixture parses");
    StyleSheet positionedCss;Check(positionedCss.Parse(positionedDoc.StyleText(),&error),L"positioned layout CSS parses");
    const auto overlayStyle=positionedCss.Compute(positionedDoc.GetElementById(L"overlay"));
    Check(overlayStyle.Get(L"top")==L"0"&&overlayStyle.Get(L"right")==L"0"&&
          overlayStyle.Get(L"bottom")==L"0"&&overlayStyle.Get(L"left")==L"0"&&
          overlayStyle.Get(L"align-items")==L"center"&&overlayStyle.Get(L"justify-items")==L"center",
          L"inset and place-items shorthands populate positioned layout longhands");
    LayoutEngine positionedLayout(positionedDoc,positionedCss);positionedLayout.Layout(300,200);
    const auto* overlayBox=FindLayout(positionedLayout.Root(),L"overlay");
    const auto* dialogBox=FindLayout(positionedLayout.Root(),L"dialog");
    Check(overlayBox&&dialogBox&&std::lround(overlayBox->rect.width)==300&&std::lround(overlayBox->rect.height)==200&&
          std::lround(dialogBox->rect.x)==130&&std::lround(dialogBox->rect.y)==80,
          L"fixed inset sizing, grid centering and percentage transforms compose generically");

    Check(std::abs(StyleSheet::Length(L"min(560px, calc(100vw - 48px))",1101,1101,-1)-560)<0.01f&&
          std::abs(StyleSheet::Length(L"max(240px, 30%)",1000,1000,-1)-300)<0.01f&&
          std::abs(StyleSheet::Length(L"clamp(240px, 50%, 560px)",800,800,-1)-400)<0.01f,
          L"CSS min, max and clamp math functions resolve nested lengths");
    Document responsiveDialogDoc;
    Check(responsiveDialogDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.overlay{position:fixed;inset:0;display:grid;place-items:center;padding:24px}.dialog{width:min(560px,calc(100vw - 48px));height:250px}</style>"
          L"<div class='overlay'><section id='responsive-dialog' class='dialog'></section></div>",&error),
          L"responsive dialog fixture parses");
    StyleSheet responsiveDialogCss;Check(responsiveDialogCss.Parse(responsiveDialogDoc.StyleText(),&error),L"responsive dialog CSS parses");
    LayoutEngine responsiveDialogLayout(responsiveDialogDoc,responsiveDialogCss);responsiveDialogLayout.Layout(1101,719);
    const auto* responsiveDialog=FindLayout(responsiveDialogLayout.Root(),L"responsive-dialog");
    Check(responsiveDialog&&std::abs(responsiveDialog->rect.width-560)<0.01f&&
          std::abs(responsiveDialog->rect.x-270.5f)<0.01f&&std::abs(responsiveDialog->rect.y-234.5f)<0.01f,
          L"grid items resolve responsive explicit sizes against their containing grid area");

    Document implicitScrollDoc;
    Check(implicitScrollDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.grid{display:grid;width:300px;height:100px}.scroll{min-height:0;overflow:auto;scrollbar-width:thin;scrollbar-color:#4a4a4a transparent}.scroll::-webkit-scrollbar{width:10px}.scroll::-webkit-scrollbar-track{background:transparent}.scroll::-webkit-scrollbar-thumb{min-height:36px;background:#454545;border:3px solid transparent;border-radius:99px}.row{height:60px}</style>"
          L"<div class='grid'><main id='implicit-scroll' class='scroll'><div class='row'>1</div><div class='row'>2</div><div class='row'>3</div></main></div>",&error),
          L"implicit grid scroll fixture parses");
    StyleSheet implicitScrollCss;Check(implicitScrollCss.Parse(implicitScrollDoc.StyleText(),&error),L"implicit grid scroll CSS parses");
    LayoutEngine implicitScrollLayout(implicitScrollDoc,implicitScrollCss);implicitScrollLayout.Layout(300,100);
    const auto implicitScrollNode=implicitScrollDoc.GetElementById(L"implicit-scroll");
    const auto* implicitScrollBox=implicitScrollLayout.BoxFor(implicitScrollNode);
    const auto implicitThumbStyle=implicitScrollBox?
        implicitScrollCss.Compute(implicitScrollNode,&implicitScrollBox->style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
    Check(implicitScrollBox&&std::lround(implicitScrollBox->rect.height)==100&&
          std::lround(implicitScrollBox->scrollHeight)==180,
          L"an overflow-auto item shrinks inside a definite implicit grid row");
    std::shared_ptr<Node> thinScrollbarDrag;float thinScrollbarOffset=0;
    const float implicitScrollRight=implicitScrollBox?implicitScrollBox->content.x+implicitScrollBox->content.width:0;
    const float implicitScrollTop=implicitScrollBox?implicitScrollBox->content.y:0;
    Check(implicitScrollBox&&implicitScrollBox->style.Get(L"scrollbar-width")==L"thin"&&
          implicitScrollBox->style.Get(L"scrollbar-color")==L"#4a4a4a transparent"&&
          implicitThumbStyle.Get(L"background-color")==L"#454545"&&
          implicitThumbStyle.Get(L"min-height")==L"36px"&&
          !implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-12,implicitScrollTop+5,thinScrollbarDrag,thinScrollbarOffset)&&
          implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-5,implicitScrollTop+5,thinScrollbarDrag,thinScrollbarOffset)&&
          !thinScrollbarDrag&&
          implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-5,implicitScrollTop+17,thinScrollbarDrag,thinScrollbarOffset)&&
          thinScrollbarDrag==implicitScrollNode,
          L"thin scrollbar styling uses a ten-pixel track with compact triangle buttons and an inset thumb");
    Check(implicitScrollLayout.ScrollAt(150,50,-120)&&implicitScrollNode->scrollTop>0,
          L"the mouse wheel scrolls an overflow-auto item in an implicit grid row");

    Document gridScrollerDoc;
    Check(gridScrollerDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.scroller{display:grid;width:300px;height:100px;overflow:auto;grid-template-columns:100px 1fr}.tall{height:150px}</style>"
          L"<div id='grid-scroller' class='scroller'><div class='tall'></div><div id='grid-scroll-second'></div></div>",&error),
          L"scrolling grid fixture parses");
    StyleSheet gridScrollerCss;Check(gridScrollerCss.Parse(gridScrollerDoc.StyleText(),&error),L"scrolling grid CSS parses");
    LayoutEngine gridScrollerLayout(gridScrollerDoc,gridScrollerCss);gridScrollerLayout.Layout(400,200);
    const auto* gridScroller=FindLayout(gridScrollerLayout.Root(),L"grid-scroller");
    const auto* gridScrollSecond=FindLayout(gridScrollerLayout.Root(),L"grid-scroll-second");
    Check(gridScroller&&gridScrollSecond&&std::lround(gridScroller->scrollHeight)==150&&
          std::lround(gridScrollSecond->rect.width)==185,
          L"overflowing grid containers compute scroll height and reserve DPI-independent scrollbar space");

    Document stableGutterDoc;
    Check(stableGutterDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.gutter{width:200px;height:80px;overflow-y:auto;scrollbar-gutter:stable}.child{width:100%;height:20px}</style>"
          L"<div class='gutter'><div id='stable-gutter-child' class='child'></div></div>",&error),
          L"stable scrollbar-gutter fixture parses");
    StyleSheet stableGutterCss;Check(stableGutterCss.Parse(stableGutterDoc.StyleText(),&error),L"stable scrollbar-gutter CSS parses");
    LayoutEngine stableGutterLayout(stableGutterDoc,stableGutterCss);stableGutterLayout.Layout(240,100);
    const auto* stableGutterChild=FindLayout(stableGutterLayout.Root(),L"stable-gutter-child");
    Check(stableGutterChild&&std::lround(stableGutterChild->rect.width)==185,
          L"scrollbar-gutter stable reserves a DPI-independent gutter before overflow appears");

    Document popupMenuDoc;
    Check(popupMenuDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}#bar{display:flex;align-items:center;width:320px;height:42px;border-bottom:1px solid #444}#menu{display:flex;align-items:center;height:100%}#menu-root{position:relative;height:100%}#trigger{display:inline-grid;place-items:center;height:100%;padding:0 11px}.popup{display:none;position:absolute;top:100%;width:100px;height:30px}.open .popup{display:block}</style>"
          L"<header id='bar'><nav id='menu'><div id='menu-root'><button id='trigger'>File</button><div class='popup'></div></div></nav></header>",&error),
          L"popup menu layout fixture parses");
    StyleSheet popupMenuCss;Check(popupMenuCss.Parse(popupMenuDoc.StyleText(),&error),L"popup menu CSS parses");
    LayoutEngine popupMenuLayout(popupMenuDoc,popupMenuCss);popupMenuLayout.Layout(320,80);
    const auto popupMenuRoot=popupMenuDoc.GetElementById(L"menu-root");
    const auto* closedTrigger=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"trigger"));
    const auto* closedMenu=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"menu"));
    const bool closedMenuValid=closedTrigger&&closedMenu;
    const float closedTriggerY=closedTrigger?closedTrigger->rect.y:-1;
    const float closedTriggerHeight=closedTrigger?closedTrigger->rect.height:-1;
    if(popupMenuRoot)popupMenuRoot->AddClass(L"open");
    popupMenuLayout.Layout(320,80);
    const auto* openTrigger=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"trigger"));
    const auto* openMenu=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"menu"));
    Check(closedMenuValid&&openTrigger&&openMenu&&
          std::abs(closedTriggerY-openTrigger->rect.y)<0.01f&&
          std::abs(closedTriggerHeight-openTrigger->rect.height)<0.01f&&
          std::abs(openTrigger->rect.height-openMenu->content.height)<0.01f,
          L"opening an absolute popup keeps its full-height menu trigger in place");

    Document pointerHitDoc;
    Check(pointerHitDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.stage{position:relative;width:200px;height:100px}.target{width:60px;height:30px}.overlay{position:absolute;inset:0;pointer-events:none}.host{position:relative;width:40px;height:20px}.popup{position:absolute;left:0;top:20px;width:100px;height:40px}.stack{position:relative;width:100px;height:50px;margin-left:150px}.front,.back{position:absolute;inset:0}.front{z-index:2}</style>"
          L"<div class='stage'><button id='pointer-target' class='target'>Target</button><div class='overlay'></div></div><div class='host'><button id='overflow-popup' class='popup'>Popup</button></div><div class='stack'><button id='z-front' class='front'>Front</button><button id='z-back' class='back'>Back</button></div>",&error),
          L"pointer hit-testing fixture parses");
    StyleSheet pointerHitCss;Check(pointerHitCss.Parse(pointerHitDoc.StyleText(),&error),L"pointer hit-testing CSS parses");
    LayoutEngine pointerHitLayout(pointerHitDoc,pointerHitCss);pointerHitLayout.Layout(300,220);
    const auto pointerTarget=pointerHitDoc.GetElementById(L"pointer-target");
    const auto overflowPopup=pointerHitDoc.GetElementById(L"overflow-popup");
    const auto zFront=pointerHitDoc.GetElementById(L"z-front");
    const auto* pointerTargetBox=pointerHitLayout.BoxFor(pointerTarget);
    const auto* overflowPopupBox=pointerHitLayout.BoxFor(overflowPopup);
    Check(pointerTargetBox&&pointerHitLayout.HitTest(pointerTargetBox->rect.x+10,pointerTargetBox->rect.y+10)==pointerTarget,
          L"pointer-events none overlay lets pointer input reach the element underneath");
    Check(overflowPopupBox&&pointerHitLayout.HitTest(overflowPopupBox->rect.x+10,overflowPopupBox->rect.y+10)==overflowPopup,
          L"overflow-visible positioned descendants remain clickable outside the parent border");
    const auto* zFrontBox=pointerHitLayout.BoxFor(zFront);
    Check(zFrontBox&&pointerHitLayout.HitTest(zFrontBox->rect.x+10,zFrontBox->rect.y+10)==zFront,
          L"hit testing follows z-index stacking instead of raw DOM order");

    Document nestedStackingDoc;
    Check(nestedStackingDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.root{position:relative;width:200px;height:100px}.early{position:relative;width:40px;height:20px}.popup{position:absolute;left:0;top:20px;width:100px;height:40px;z-index:3}.cover{position:absolute;left:0;top:20px;width:100px;height:40px}</style>"
          L"<div class='root'><div class='early'><button id='nested-popup' class='popup'>Popup</button></div><button id='later-cover' class='cover'>Cover</button></div>",&error),
          L"nested stacking-context fixture parses");
    StyleSheet nestedStackingCss;Check(nestedStackingCss.Parse(nestedStackingDoc.StyleText(),&error),L"nested stacking-context CSS parses");
    LayoutEngine nestedStackingLayout(nestedStackingDoc,nestedStackingCss);nestedStackingLayout.Layout(200,100);
    const auto nestedPopup=nestedStackingDoc.GetElementById(L"nested-popup");
    const auto* nestedPopupBox=nestedStackingLayout.BoxFor(nestedPopup);
    Check(nestedPopupBox&&nestedStackingLayout.HitTest(nestedPopupBox->rect.x+10,nestedPopupBox->rect.y+10)==nestedPopup,
          L"a positive z-index descendant stacks above later content outside its non-stacking ancestor");

    Document popupScrollChainDoc;
    Check(popupScrollChainDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.root{position:relative;width:200px;height:100px;overflow:hidden}.early{position:relative;width:20px;height:20px}.popup{position:absolute;left:10px;top:10px;width:100px;height:50px;overflow-y:auto;z-index:3}.popup-fill{height:150px}.main-scroll{position:absolute;inset:0;overflow-y:auto}.main-fill{height:300px}</style>"
          L"<div class='root'><div class='early'><div id='popup-scroll' class='popup'><div class='popup-fill'></div></div></div><main id='covered-main-scroll' class='main-scroll'><div class='main-fill'></div></main></div>",&error),
          L"popup scroll-chain fixture parses");
    StyleSheet popupScrollChainCss;Check(popupScrollChainCss.Parse(popupScrollChainDoc.StyleText(),&error),L"popup scroll-chain CSS parses");
    LayoutEngine popupScrollChainLayout(popupScrollChainDoc,popupScrollChainCss);popupScrollChainLayout.Layout(200,100);
    const auto popupScroll=popupScrollChainDoc.GetElementById(L"popup-scroll");
    const auto coveredMainScroll=popupScrollChainDoc.GetElementById(L"covered-main-scroll");
    const auto* popupScrollBox=popupScrollChainLayout.BoxFor(popupScroll);
    const float popupScrollX=popupScrollBox?popupScrollBox->content.x+20:0;
    const float popupScrollY=popupScrollBox?popupScrollBox->content.y+20:0;
    std::shared_ptr<Node> popupScrolledNode;
    const bool popupFirstWheel=popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    const float coveredMainAfterFirstWheel=coveredMainScroll?coveredMainScroll->scrollTop:-1;
    popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    popupScrolledNode.reset();
    const bool popupBoundaryWheel=popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    Check(popupScrollBox&&popupFirstWheel&&popupScrolledNode==nullptr&&
          popupScroll->scrollTop>=popupScrollBox->scrollHeight-popupScrollBox->content.height-0.01f&&
          coveredMainAfterFirstWheel==0&&coveredMainScroll&&coveredMainScroll->scrollTop==0&&!popupBoundaryWheel,
          L"wheel input over a popup scrolls only its hit-target chain and never a covered sibling at the boundary");

    Document fixedInsetDoc;
    Check(fixedInsetDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}body{display:grid}.fixed{position:fixed;right:18px;bottom:18px;width:50px;height:20px}</style><div id='fixed-inset' class='fixed'></div>",&error),
          L"fixed right-bottom inset fixture parses");
    StyleSheet fixedInsetCss;Check(fixedInsetCss.Parse(fixedInsetDoc.StyleText(),&error),L"fixed right-bottom inset CSS parses");
    LayoutEngine fixedInsetLayout(fixedInsetDoc,fixedInsetCss);fixedInsetLayout.Layout(300,200);
    const auto* fixedInset=FindLayout(fixedInsetLayout.Root(),L"fixed-inset");
    Check(fixedInset&&std::lround(fixedInset->rect.x)==232&&std::lround(fixedInset->rect.y)==162,
          L"fixed descendants of grid containers honor right and bottom insets");

    Document transitionDoc;
    Check(transitionDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.workspace{width:300px;height:100px;display:grid;grid-template-columns:200px minmax(0,1fr);transition:grid-template-columns 200ms linear}.workspace.closed{grid-template-columns:0 minmax(0,1fr)}.panel{min-width:0;overflow:hidden;opacity:1;visibility:visible;transform:translateX(0);transition:opacity 100ms linear,transform 200ms linear,visibility 0s linear 0s}.closed .panel{opacity:0;visibility:hidden;transform:translateX(-20px);transition:opacity 100ms linear,transform 200ms linear,visibility 0s linear 200ms}</style>"
          L"<div id='transition-workspace' class='workspace'><aside id='transition-panel' class='panel'></aside><main></main></div>",&error),
          L"CSS transition fixture parses");
    StyleSheet transitionCss;Check(transitionCss.Parse(transitionDoc.StyleText(),&error),L"CSS transition stylesheet parses");
    LayoutEngine transitionLayout(transitionDoc,transitionCss);transitionLayout.Layout(300,100);
    auto transitionWorkspace=transitionDoc.GetElementById(L"transition-workspace");
    transitionWorkspace->SetAttribute(L"class",L"workspace closed");transitionLayout.Layout(300,100);
    const auto* transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.HasActiveTransitions()&&transitionPanel&&std::lround(transitionPanel->rect.width)==200&&
          std::lround(transitionPanel->rect.x)==0&&transitionPanel->style.Get(L"visibility")==L"visible",
          L"CSS transitions retain the starting grid, transform and delayed visibility on the first frame");
    const auto* retainedTransitionRoot=transitionLayout.Root();
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.Root()==retainedTransitionRoot&&transitionPanel&&std::abs(transitionPanel->rect.width-100)<0.1f&&
          std::abs(transitionPanel->rect.x+10)<0.1f&&
          std::abs(std::stof(transitionPanel->style.Get(L"opacity")))<0.01f&&
          transitionPanel->style.Get(L"visibility")==L"visible",
          L"CSS transitions reuse the box tree while interpolating grid, transform and opacity values");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(!transitionLayout.HasActiveTransitions()&&transitionPanel&&transitionPanel->rect.width<0.1f&&
          transitionPanel->style.Get(L"visibility")==L"hidden",
          L"CSS transitions commit their target values and delayed discrete state at completion");
    transitionWorkspace->SetAttribute(L"class",L"workspace");transitionLayout.Layout(300,100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.HasActiveTransitions()&&transitionPanel&&transitionPanel->rect.width<0.1f&&
          transitionPanel->style.Get(L"visibility")==L"visible",
          L"reverse CSS transitions start from the collapsed geometry and reveal immediately");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionPanel&&std::abs(transitionPanel->rect.width-100)<0.1f&&
          std::abs(transitionPanel->rect.x+10)<0.1f,
          L"reverse CSS transitions interpolate back toward the expanded layout");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(!transitionLayout.HasActiveTransitions()&&transitionPanel&&std::abs(transitionPanel->rect.width-200)<0.1f&&
          std::abs(transitionPanel->rect.x)<0.1f,
          L"reverse CSS transitions finish at the expanded layout");

    Document buttonDoc;
    Check(buttonDoc.Parse(L"<style>#override{text-align:right}</style><div id='parent'><button id='default'>Default</button><button id='override'>Override</button></div>",&error),
          L"button alignment fixture parses");
    StyleSheet buttonCss;Check(buttonCss.Parse(buttonDoc.StyleText(),&error),L"button alignment CSS parses");
    const auto parentButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"parent"));
    const auto defaultButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"default"),&parentButtonStyle);
    const auto overrideButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"override"),&parentButtonStyle);
    Check(defaultButtonStyle.Get(L"text-align")==L"center"&&defaultButtonStyle.Get(L"white-space")==L"nowrap",
          L"button user-agent alignment and no-wrap behavior override inherited defaults");
    Check(overrideButtonStyle.Get(L"text-align")==L"right",L"author text alignment overrides the button user-agent default");

    Document emptyInputCaretDoc;
    Check(emptyInputCaretDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}input{display:block;width:180px;height:32px;padding:0 9px;border:1px solid #ccc;font:13px/1.42 'Segoe UI'}</style>"
          L"<input id='empty-password' type='password'><input id='filled-password' type='password' value='x'>",
          &error),L"empty input caret fixture parses");
    StyleSheet emptyInputCaretCss;
    Check(emptyInputCaretCss.Parse(emptyInputCaretDoc.StyleText(),&error),
          L"empty input caret CSS parses");
    LayoutEngine emptyInputCaretLayout(emptyInputCaretDoc,emptyInputCaretCss);
    emptyInputCaretLayout.Layout(240,100);
    const auto emptyPassword=emptyInputCaretDoc.GetElementById(L"empty-password");
    const auto filledPassword=emptyInputCaretDoc.GetElementById(L"filled-password");
    const auto* emptyPasswordBox=emptyInputCaretLayout.BoxFor(emptyPassword);
    const auto* filledPasswordBox=emptyInputCaretLayout.BoxFor(filledPassword);
    LayoutRect emptyPasswordCaret{},filledPasswordCaret{};
    const bool hasEmptyPasswordCaret=emptyInputCaretLayout.TextCaretRect(
        emptyPassword,0,emptyPasswordCaret);
    const bool hasFilledPasswordCaret=emptyInputCaretLayout.TextCaretRect(
        filledPassword,0,filledPasswordCaret);
    Check(hasEmptyPasswordCaret&&hasFilledPasswordCaret&&emptyPasswordBox&&filledPasswordBox&&
          std::abs((emptyPasswordCaret.y-emptyPasswordBox->content.y)-
                   (filledPasswordCaret.y-filledPasswordBox->content.y))<0.01f&&
          std::abs(emptyPasswordCaret.height-filledPasswordCaret.height)<0.01f,
          L"an empty single-line input caret uses the same centered line box as entered text");

    Document intrinsicMenuDoc;
    Check(intrinsicMenuDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}.popup{position:absolute;width:max-content;min-width:220px;padding:5px}"
        L".popup button{display:block;width:100%;padding:5px 28px 5px 10px;white-space:pre;font:12px Arial;text-align:left}"
        L".tab-probe{display:inline-block;white-space:pre;font:16px Arial}.tab-two{tab-size:2}.tab-eight{tab-size:8}</style>"
        L"<div id='intrinsic-popup' class='popup'><button>Save document under another name...\tCtrl+Shift+S</button></div>"
        L"<span id='tab-two' class='tab-probe tab-two'>A\tB</span>"
        L"<span id='tab-eight' class='tab-probe tab-eight'>A\tB</span>",
        &error),L"intrinsic menu and tab-size fixture parses");
    StyleSheet intrinsicMenuCss;
    Check(intrinsicMenuCss.Parse(intrinsicMenuDoc.StyleText(),&error),
          L"intrinsic menu and tab-size CSS parses");
    LayoutEngine intrinsicMenuLayout(intrinsicMenuDoc,intrinsicMenuCss);
    intrinsicMenuLayout.Layout(800,200);
    const auto* intrinsicPopup=FindLayout(intrinsicMenuLayout.Root(),L"intrinsic-popup");
    const auto* tabTwo=FindLayout(intrinsicMenuLayout.Root(),L"tab-two");
    const auto* tabEight=FindLayout(intrinsicMenuLayout.Root(),L"tab-eight");
    Check(intrinsicPopup&&intrinsicPopup->rect.width>300,
          L"width max-content uses the widest unwrapped child instead of the minimum width");
    if(tabTwo&&tabEight&&tabEight->rect.width<=tabTwo->rect.width+10)
        std::wcerr<<L"tab-size widths: "<<tabTwo->rect.width<<L", "<<tabEight->rect.width<<L"\n";
    Check(tabTwo&&tabEight&&tabEight->rect.width>tabTwo->rect.width+10,
          L"tab-size changes preserved-tab text measurement and inline intrinsic width");
    const auto tabText=intrinsicMenuDoc.QuerySelector(L"#tab-eight")->children.front();
    const auto tabTextStyle=intrinsicMenuCss.Compute(tabText,&tabEight->style);
    Check(tabTextStyle.Get(L"tab-size")==L"8",L"tab-size inherits into text runs");

    Document checkDoc;
    Check(checkDoc.Parse(L"<style>html,body{margin:0}*{box-sizing:border-box}.check{width:100px;height:25px;display:flex;align-items:center;gap:7px}.check input{width:15px;height:15px}</style><label class='check'><input id='check' type='checkbox'><span id='check-label'>Choice</span></label>",&error),
          L"checkbox user-agent fixture parses");
    StyleSheet checkCss;Check(checkCss.Parse(checkDoc.StyleText(),&error),L"checkbox user-agent CSS parses");
    const auto checkStyle=checkCss.Compute(checkDoc.GetElementById(L"check"));
    Check(checkStyle.Get(L"margin")==L"3px 3px 3px 4px",
          L"checkbox keeps Chromium user-agent margins when author CSS sets its size");
    LayoutEngine checkLayout(checkDoc,checkCss);checkLayout.Layout(200,60);
    const auto* checkBox=FindLayout(checkLayout.Root(),L"check");
    const auto* checkLabel=FindLayout(checkLayout.Root(),L"check-label");
    Check(checkBox&&checkLabel&&std::lround(checkBox->rect.x)==4&&
          std::lround(checkLabel->rect.x)==29,
          L"checkbox margins and flex gap place following label text like Chromium");

    Document fixedDoc;
    Check(fixedDoc.Parse(L"<style>*{margin:0;padding:0}table{width:300px;table-layout:fixed}th:first-child{width:50px}th:last-child{width:100px}td:first-child{width:250px}</style><table><thead><tr><th>A</th><th>B</th></tr></thead><tbody><tr><td>Long second-row content</td><td>C</td></tr></tbody></table>",&error),
          L"fixed table fixture parses");
    StyleSheet fixedCss;Check(fixedCss.Parse(fixedDoc.StyleText(),&error),L"fixed table CSS parses");
    LayoutEngine fixedLayout(fixedDoc,fixedCss);fixedLayout.Layout(400,200);
    const auto* firstFixedColumn=fixedLayout.BoxFor(fixedDoc.QuerySelector(L"thead th:first-child"));
    const auto* secondFixedColumn=fixedLayout.BoxFor(fixedDoc.QuerySelector(L"thead th:last-child"));
    Check(firstFixedColumn&&secondFixedColumn&&std::lround(firstFixedColumn->rect.width)==100&&
          std::lround(secondFixedColumn->rect.width)==200,
          L"fixed table columns use the first row instead of later content widths");

    Document doc; error.clear();
    Check(doc.Parse(L"<html><head><style>#x{color:red}.on{display:flex}</style></head><body><div id='x' class='on'>hello <span>world</span></div></body></html>", &error), L"HTML parses");
    Check(doc.GetElementById(L"x") != nullptr, L"id index");
    Check(doc.QuerySelectorAll(L"#x.on span").size() == 1, L"descendant selector");
    StyleSheet css; Check(css.Parse(doc.StyleText(), &error), L"CSS parses");
    auto style = css.Compute(doc.GetElementById(L"x"));
    Check(style.Is(L"display", L"flex"), L"CSS cascade");
    Document focusWithinDoc;Check(focusWithinDoc.Parse(L"<label class='field'><input id='query'></label>",&error),L"focus-within fixture parses");
    const auto focusWithinInput=focusWithinDoc.GetElementById(L"query");
    const auto focusWithinLabel=focusWithinDoc.QuerySelector(L".field");
    focusWithinInput->focused=true;focusWithinInput->focusWithin=true;focusWithinLabel->focusWithin=true;
    Check(Document::MatchesSelector(focusWithinLabel,L".field:focus-within"),L"focus-within matches an ancestor of the focused control");
    focusWithinInput->focused=false;focusWithinInput->focusWithin=false;focusWithinLabel->focusWithin=false;
    Check(!Document::MatchesSelector(focusWithinLabel,L".field:focus-within"),L"focus-within clears when no descendant is focused");

    Document alignmentDoc;Check(alignmentDoc.Parse(
        L"<style>*{box-sizing:border-box}button{display:inline-flex;align-items:center;gap:7px;min-height:36px;padding:0 13px;font-family:'Segoe UI';font-size:14px}.icon{width:15px;height:15px}.copy{display:grid;gap:1px}.copy strong{font-size:13px}.copy small{font-size:11px}</style><body><button id='aligned-button'>\n  <svg class='icon' viewBox='0 0 24 24'></svg> Export\n</button><span id='stacked-copy' class='copy'><strong>Account</strong><small>Developer plan</small></span></body>",
        &error),L"generic flex and grid alignment fixture parses");
    StyleSheet alignmentCss;Check(alignmentCss.Parse(alignmentDoc.StyleText(),&error),L"generic flex and grid alignment CSS parses");
    LayoutEngine alignmentLayout(alignmentDoc,alignmentCss);alignmentLayout.Layout(400,200);
    const auto* alignedButton=FindLayout(alignmentLayout.Root(),L"aligned-button");
    const auto* stackedCopy=FindLayout(alignmentLayout.Root(),L"stacked-copy");
    if(alignedButton&&alignedButton->children.size()>=2){
        const auto& icon=*alignedButton->children[0];const auto& label=*alignedButton->children[1];
        Check(alignedButton->style.Is(L"white-space",L"nowrap")&&
              std::abs((icon.rect.y+icon.rect.height/2)-(label.rect.y+label.rect.height/2))<0.01f,
              L"formatted flex-button HTML collapses source whitespace and vertically centers icon and label boxes");
    }
    if(stackedCopy&&stackedCopy->children.size()>=2){
        const auto& first=*stackedCopy->children[0];const auto& second=*stackedCopy->children[1];
        Check(second.style.Is(L"display",L"inline")&&second.rect.height<19&&
              second.rect.y>=first.rect.y+first.rect.height+0.9f,
              L"small is an inline phrasing element and stacked grid text rows do not overlap");
    }

    Document baselineDoc;
    Check(baselineDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.line{display:flex;align-items:baseline;width:200px;height:30px}.small{font-family:'Segoe UI';font-size:10px}.large{font-family:'Segoe UI';font-size:14px}</style><div class='line'><span id='baseline-small' class='small'>SMALL</span><strong id='baseline-large' class='large'>Large</strong></div>",
          &error),L"flex baseline fixture parses");
    StyleSheet baselineCss;Check(baselineCss.Parse(baselineDoc.StyleText(),&error),
          L"flex baseline CSS parses");
    LayoutEngine baselineLayout(baselineDoc,baselineCss);baselineLayout.Layout(240,80);
    const auto* baselineSmall=FindLayout(baselineLayout.Root(),L"baseline-small");
    const auto* baselineLarge=FindLayout(baselineLayout.Root(),L"baseline-large");
    Check(baselineSmall&&baselineLarge&&baselineSmall->rect.y>baselineLarge->rect.y+2.0f&&
          baselineSmall->rect.y+baselineSmall->rect.height<=
              baselineLarge->rect.y+baselineLarge->rect.height+0.5f,
          L"row flex items align their first text baselines across font sizes");

    Document inlineDoc;
    Check(inlineDoc.Parse(L"<style>#bar,#header{display:flex;align-items:center;height:54px}#bar,#spaces{font-size:12px}#header{font-size:15px}#icon{display:inline-flex;width:15px}#spaces{display:flex;gap:8px}</style><div id='bar'><strong id='simple'>DISCONNECTED</strong><span id='mixed'>available: <strong>00</strong></span></div><div id='header'><strong id='title'><span id='icon'>x</span>Saved Data</strong></div><div id='spaces'><strong id='trail'>LINK: </strong><strong>DISCONNECTED</strong><strong id='plain'>LINK:</strong></div>", &error),
          L"inline formatting fixture parses");
    StyleSheet inlineCss; Check(inlineCss.Parse(inlineDoc.StyleText(), &error), L"inline formatting CSS parses");
    LayoutEngine inlineLayout(inlineDoc,inlineCss);inlineLayout.Layout(400,120);
    const auto* simpleInline=FindLayout(inlineLayout.Root(),L"simple");
    const auto* mixedInline=FindLayout(inlineLayout.Root(),L"mixed");
    const auto* titleInline=FindLayout(inlineLayout.Root(),L"title");
    const auto* iconInline=FindLayout(inlineLayout.Root(),L"icon");
    const auto* trailingInline=FindLayout(inlineLayout.Root(),L"trail");
    const auto* plainInline=FindLayout(inlineLayout.Root(),L"plain");
    Check(simpleInline&&mixedInline&&std::lround(simpleInline->rect.height)==16&&
          std::lround(mixedInline->rect.height)==16&&std::abs(simpleInline->rect.y-mixedInline->rect.y)<0.01f,
          L"nested inline content uses the inherited line box instead of an arbitrary minimum height");
    Check(titleInline&&iconInline&&titleInline->children.size()==2&&
          std::abs(iconInline->rect.y-titleInline->children[1]->rect.y)<0.01f&&
          iconInline->rect.x<titleInline->children[1]->rect.x,
          L"inline-flex participates in its parent's inline formatting line");
    Check(trailingInline&&plainInline&&std::abs(trailingInline->rect.width-plainInline->rect.width)<0.01f,
          L"collapsible trailing whitespace is removed at the end of an inline formatting context");

    Document leadingSpaceDoc;
    Check(leadingSpaceDoc.Parse(L"<style>label{display:block;font-size:11px}</style><label id='with-space'><input type='checkbox'> filename</label><label id='without-space'><input type='checkbox'>filename</label>",&error),
          L"leading inline whitespace fixture parses");
    StyleSheet leadingSpaceCss;Check(leadingSpaceCss.Parse(leadingSpaceDoc.StyleText(),&error),L"leading inline whitespace CSS parses");
    LayoutEngine leadingSpaceLayout(leadingSpaceDoc,leadingSpaceCss);leadingSpaceLayout.Layout(400,100);
    const auto* withSpace=FindLayout(leadingSpaceLayout.Root(),L"with-space");
    const auto* withoutSpace=FindLayout(leadingSpaceLayout.Root(),L"without-space");
    const LayoutBox* withSpaceText=withSpace&&withSpace->children.size()==2?withSpace->children[1].get():nullptr;
    const LayoutBox* withoutSpaceText=withoutSpace&&withoutSpace->children.size()==2?withoutSpace->children[1].get():nullptr;
    Check(withSpaceText&&withoutSpaceText&&withSpaceText->rect.width-withoutSpaceText->rect.width>2.5f,
          L"collapsed leading whitespace is preserved between inline siblings");

    Document clippedTextDoc;
    Check(clippedTextDoc.Parse(L"<style>*{box-sizing:border-box}.cell{width:120px;height:28px;padding:0 10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.right{text-align:right}</style><div id='clipped-left' class='cell'>A very long single line</div><div id='clipped-right' class='cell right'>42</div>",&error),
          L"single-line clipped text fixture parses");
    StyleSheet clippedTextCss;Check(clippedTextCss.Parse(clippedTextDoc.StyleText(),&error),L"single-line clipped text CSS parses");
    LayoutEngine clippedTextLayout(clippedTextDoc,clippedTextCss);clippedTextLayout.Layout(300,100);
    const auto* clippedLeft=FindLayout(clippedTextLayout.Root(),L"clipped-left");
    const auto* clippedRight=FindLayout(clippedTextLayout.Root(),L"clipped-right");
    Check(clippedLeft&&clippedLeft->children.size()==1&&
          std::abs(clippedLeft->children[0]->rect.x-clippedLeft->content.x)<0.01f&&
          std::abs(clippedLeft->children[0]->rect.width-clippedLeft->content.width)<0.01f&&
          std::abs(clippedLeft->children[0]->rect.height-clippedLeft->content.height)<0.01f,
          L"single-line clipped text uses its containing block without intrinsic-width layout");
    Check(clippedRight&&clippedRight->children.size()==1&&
          std::abs(clippedRight->children[0]->rect.width-clippedRight->content.width)<0.01f,
          L"single-line clipped text preserves a full alignment rectangle");

    JavaScriptRuntime js(doc); std::wstring message;
    js.SetMessageSink([&](const std::wstring& value) { message = value; });
    const wchar_t* source = LR"JS(
        function update(data) {
            const x = document.getElementById('x');
            x.innerText = `${data.name}:${(data.value * 2).toFixed(1)}`;
            x.classList.add('changed');
        }
        document.addEventListener('DOMContentLoaded', () => {
            document.getElementById('x').addEventListener('click', function(e) {
                window.chrome.webview.postMessage('clicked:' + this.innerText);
            });
        });
    )JS";
    Check(js.Load(source, &error), error.c_str());
    js.DispatchDocumentEvent(L"DOMContentLoaded");
    Check(js.Execute(L"update({name:'A', value:2.5});", nullptr, &error), error.c_str());
    Check(doc.GetElementById(L"x")->InnerText() == L"A:5.0", L"compiled function mutates DOM");
    Check(doc.GetElementById(L"x")->HasClass(L"changed"), L"classList binding");
    js.DispatchNodeEvent(doc.GetElementById(L"x"), L"click");
    Check(message == L"clicked:A:5.0", L"host message bridge");
    Check(js.Execute(L"document.getElementById('x').innerText='pending'; const skipped=requestAnimationFrame(()=>{document.getElementById('x').innerText='wrong';}); cancelAnimationFrame(skipped); requestAnimationFrame(()=>{document.getElementById('x').innerText='done';});",nullptr,&error),error.c_str());
    Check(doc.GetElementById(L"x")->InnerText()==L"pending",L"animation callbacks wait until the next frame");
    js.RunAnimationFrame();
    Check(doc.GetElementById(L"x")->InnerText()==L"done",L"animation frame runs active callbacks and skips canceled callbacks");

    Document modernJsDoc;
    Check(modernJsDoc.Parse(L"<div class='card' data-table-id='alpha'></div>",&error),L"modern JavaScript DOM fixture parses");
    JavaScriptRuntime modernJs(modernJsDoc);std::wstring modernMessage;
    modernJs.SetMessageSink([&](const std::wstring& value){modernMessage=value;});
    const wchar_t* modernSource=LR"JS(
        class Counter {
            static STEP = 2;
            constructor(root) { this.root = root; this.value = 1; this.bump = this.add.bind(this); }
            add() { this.value += Counter.STEP; return this.value; }
            schedule() { requestAnimationFrame(() => { this.root.textContent = `${this.bump()}`; this.root.dataset.state = "ready"; }); }
        }
        const base = { command: "modern" };
        const payload = { ...base, tableId: document.querySelector(".card").dataset.tableId };
        const instances = new Map();
        document.querySelectorAll(".card").forEach((card) => { const item = new Counter(card); instances.set(item.root.dataset.tableId, item); });
        for (const item of instances.values()) { item.schedule(); }
        window.chrome.webview.postMessage(JSON.stringify(payload));
    )JS";
    Check(modernJs.Load(modernSource,&error),error.c_str());
    Check(modernMessage.find(L"\"command\":\"modern\"")!=std::wstring::npos&&modernMessage.find(L"\"tableId\":\"alpha\"")!=std::wstring::npos,
          L"classes, construction, object spread and dataset work together");
    modernJs.RunAnimationFrame();
    const auto modernCard=modernJsDoc.QuerySelector(L".card");
    Check(modernCard&&modernCard->InnerText()==L"3"&&modernCard->Attribute(L"data-state")==L"ready",
          L"bound methods, lexical arrow this and for-of iterables run on animation frames");

    Document hostTableDoc;
    Check(hostTableDoc.Parse(L"<table><tbody id='rows'></tbody></table>",&error),
          L"host-fed table DOM fixture parses");
    JavaScriptRuntime hostTableJs(hostTableDoc);
    const wchar_t* hostTableSource=LR"JS(
        function escapeHtml(value) {
            return String(value)
                .replaceAll('&','&amp;')
                .replaceAll('<','&lt;')
                .replaceAll('>','&gt;')
                .replaceAll('"','&quot;')
                .replaceAll("'","&#39;");
        }
        function renderTable(rows) {
            const body = document.getElementById('rows');
            body.innerHTML = '';
            for (const row of rows) {
                const tr = document.createElement('tr');
                tr.innerHTML = `<td>${escapeHtml(row.id)}</td><td>${escapeHtml(row.name)}</td><td>${escapeHtml(row.value)}</td>`;
                body.appendChild(tr);
            }
        }
        window.twebframe?.addEventListener('message', (event) => {
            const message = event.data;
            if (message && message.type === 'cards' && Array.isArray(message.rows)) {
                renderTable(message.rows);
                console.log(`[WebMsg] received ${message.rows.length} rows`);
            }
        });
    )JS";
    Check(hostTableJs.Load(hostTableSource,&error),error.c_str());
    Check(hostTableJs.DispatchWebMessageAsJson(
              L"{\"type\":\"cards\",\"rows\":[{\"id\":\"001\",\"name\":\"<GPU>\",\"value\":\"A&B\"},{\"id\":\"002\",\"name\":\"Memory\",\"value\":\"64GB\"}]}",
              &error),error.c_str());
    const auto hostTableBody=hostTableDoc.GetElementById(L"rows");
    const auto hostTableRows=hostTableDoc.QuerySelectorAll(L"tr",hostTableBody);
    const auto firstHostTableCells=hostTableRows.empty()?std::vector<std::shared_ptr<Node>>{}:
        hostTableDoc.QuerySelectorAll(L"td",hostTableRows.front());
    Check(hostTableRows.size()==2&&firstHostTableCells.size()==3&&
          firstHostTableCells[0]->InnerText()==L"001"&&
          firstHostTableCells[1]->InnerText()==L"<GPU>"&&
          firstHostTableCells[2]->InnerText()==L"A&B"&&
          hostTableDoc.QuerySelectorAll(L"gpu",hostTableBody).empty(),
          L"host JSON messages render table rows through generic modern JavaScript and DOM bindings");

    Document fileDoc;
    Check(fileDoc.Parse(L"<input id='file' type='file'>",&error),L"file input fixture parses");
    auto selectedFile=fileDoc.GetElementById(L"file");
    selectedFile->files.push_back({L"sample.csv",L"text/csv",L"",4096});
    selectedFile->SetAttribute(L"value",L"C:\\fakepath\\sample.csv");
    JavaScriptRuntime fileJs(fileDoc);std::wstring fileResult;
    Check(fileJs.Execute(L"const picked=document.getElementById('file').files[0]; return picked.name+'|'+picked.size+'|'+picked.type;",&fileResult,&error)&&
          fileResult==L"sample.csv|4096|text/csv",L"file input exposes standard file name, size and type to JavaScript");
    Check(fileJs.Execute(L"document.getElementById('file').value=''; return document.getElementById('file').files.length;",&fileResult,&error)&&
          fileResult==L"0",L"clearing a file input clears the selected file list");
    std::wstring droppedName;
    fileJs.SetMessageSink([&](const std::wstring& value){droppedName=value;});
    Check(fileJs.Execute(L"document.getElementById('file').addEventListener('drop',event=>window.chrome.webview.postMessage(event.type+'|'+event.dataTransfer.files[0].name));",nullptr,&error),error.c_str());
    fileJs.DispatchFileDrop(selectedFile,{{L"dropped.csv",L"text/csv",L"",100}});
    Check(droppedName==L"drop|dropped.csv",L"file drop events expose a standard dataTransfer file list");

    Document iconButtonDoc;
    Check(iconButtonDoc.Parse(L"<style>*{box-sizing:border-box}button{display:flex;align-items:center;gap:10px;padding:0 12px}svg{width:20px;height:20px}</style><div style='display:flex;width:500px'><button id='icon-button'><svg viewBox='0 0 24 24'><path d='M4 4h16v16H4z'/></svg>Sample label</button></div>",&error),L"icon button fixture parses");
    StyleSheet iconButtonCss;Check(iconButtonCss.Parse(iconButtonDoc.StyleText(),&error),L"icon button styles parse");
    LayoutEngine iconButtonLayout(iconButtonDoc,iconButtonCss);iconButtonLayout.Layout(500,100);
    const auto* iconButton=FindLayout(iconButtonLayout.Root(),L"icon-button");
    Check(iconButton&&iconButton->children.size()==2&&
          iconButton->children.back()->rect.x+iconButton->children.back()->rect.width<=
              iconButton->content.x+iconButton->content.width+0.5f,
          L"flex button intrinsic width includes its icon, text and gap");
    Document anonymousFlexDoc;
    Check(anonymousFlexDoc.Parse(
          L"<style>button{display:inline-flex;gap:7px;padding:0}</style><button id='flex-space'>A <span>B</span></button><button id='flex-tight'>A<span>B</span></button>",
          &error),L"anonymous flex text fixture parses");
    StyleSheet anonymousFlexCss;Check(anonymousFlexCss.Parse(anonymousFlexDoc.StyleText(),&error),
          L"anonymous flex text CSS parses");
    LayoutEngine anonymousFlexLayout(anonymousFlexDoc,anonymousFlexCss);anonymousFlexLayout.Layout(300,80);
    const auto* flexSpace=anonymousFlexLayout.BoxFor(anonymousFlexDoc.GetElementById(L"flex-space"));
    const auto* flexTight=anonymousFlexLayout.BoxFor(anonymousFlexDoc.GetElementById(L"flex-tight"));
    Check(flexSpace&&flexTight&&std::abs(flexSpace->rect.width-flexTight->rect.width)<0.001f,
          L"collapsible whitespace is trimmed at anonymous flex-item boundaries");
    Document preformattedFlexDoc;
    Check(preformattedFlexDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}#pre-flex{display:flex;width:300px;height:30px;justify-content:space-between;white-space:pre}</style>"
          L"<div id='pre-flex'>\n  <span id='pre-flex-label'>Recent documents</span><span id='pre-flex-arrow'>&#8250;</span>\n</div>",
          &error),L"preformatted anonymous flex whitespace fixture parses");
    StyleSheet preformattedFlexCss;
    Check(preformattedFlexCss.Parse(preformattedFlexDoc.StyleText(),&error),
          L"preformatted anonymous flex whitespace CSS parses");
    LayoutEngine preformattedFlexLayout(preformattedFlexDoc,preformattedFlexCss);
    preformattedFlexLayout.Layout(400,80);
    const auto* preFlex=FindLayout(preformattedFlexLayout.Root(),L"pre-flex");
    const auto* preFlexLabel=FindLayout(preformattedFlexLayout.Root(),L"pre-flex-label");
    const auto* preFlexArrow=FindLayout(preformattedFlexLayout.Root(),L"pre-flex-arrow");
    Check(preFlex&&preFlexLabel&&preFlexArrow&&preFlex->children.size()==2&&
          std::abs(preFlexLabel->rect.x-preFlex->content.x)<0.01f&&
          std::abs(preFlexArrow->rect.x+preFlexArrow->rect.width-
                   (preFlex->content.x+preFlex->content.width))<0.01f,
          L"preformatted source indentation does not create anonymous flex items or distort space-between");

    std::vector<std::wstring> compositionStages;std::wstring compositionResult;
    TextInput compositionInput({
        []{return true;},
        [&]{compositionStages.push_back(L"start");},
        [&](const std::wstring& text,const std::vector<unsigned char>&,size_t cursor){
            compositionStages.push_back(text+L"@"+std::to_wstring(cursor));
        },
        [&](const std::wstring& text){compositionResult=text;},
        [&]{compositionStages.push_back(L"cancel");},
        []{return RECT{10,10,11,28};}
    });
    compositionInput.StartComposition();
    compositionInput.UpdateComposition(L"\u314e",{ATTR_INPUT},1);
    compositionInput.UpdateComposition(L"\ud558",{ATTR_TARGET_CONVERTED},1);
    Check(compositionInput.IsComposing()&&compositionStages.size()==3&&
          compositionStages[1]==L"\u314e@1"&&compositionStages[2]==L"\ud558@1",
          L"Hangul jamo and partial syllables are exposed during inline composition");
    compositionInput.CommitComposition(L"\ud55c");
    Check(!compositionInput.IsComposing()&&compositionResult==L"\ud55c",
          L"only the IME result commits the completed Hangul syllable");

    HWND inputHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|WS_VISIBLE,-10000,-10000,320,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(inputHost!=nullptr,L"off-screen input test host is created");
    if(inputHost){
        RECT inputBounds{0,0,320,120};auto inputView=TWebFrame::View::Create(inputHost,inputBounds);
        Check(inputView!=nullptr,L"TWebFrame input test view is created");
        if(inputView){
            std::wstring inputMessage;inputView->SetMessageHandler([&](const std::wstring& value){inputMessage=value;});
            const wchar_t* inputHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}input{width:120px;height:32px;padding:0 4px;border:1px solid #ccc;font-size:13px}</style><input id="editor" type="text" value="abc" maxlength="5"><script>document.addEventListener('DOMContentLoaded',function(){const editor=document.getElementById('editor');editor.addEventListener('input',function(){window.chrome.webview.postMessage('input:'+this.value);});editor.addEventListener('change',function(){window.chrome.webview.postMessage('change:'+this.value);});});</script>)HTML";
            Check(inputView->NavigateToString(inputHtml),L"text input fixture loads in a real view");
            const HWND inputWindow=inputView->Window();
            Check((GetWindowLongPtrW(inputWindow,GWL_STYLE)&WS_TABSTOP)!=0,L"embedded view participates in dialog keyboard focus");
            std::wstring inputCenter,selectionError;
            Check(inputView->ExecuteScript(
                L"const r=document.getElementById('editor').getBoundingClientRect();"
                L"return Math.round(r.x+5)+','+Math.round(r.y+r.height/2);",
                &inputCenter,&selectionError),selectionError.c_str());
            const auto inputCenterComma=inputCenter.find(L',');
            Check(inputCenterComma!=std::wstring::npos,L"text input geometry can be queried");
            if(inputCenterComma!=std::wstring::npos){
                const auto scale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
                const int x=static_cast<int>(std::lround(std::stod(inputCenter.substr(0,inputCenterComma))*scale));
                const int y=static_cast<int>(std::lround(std::stod(inputCenter.substr(inputCenterComma+1))*scale));
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            }
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"focused text input is rendered and edited without a child Win32 Edit");
            const auto dialogCode=SendMessageW(inputWindow,WM_GETDLGCODE,static_cast<WPARAM>(L'A'),0);
            Check((dialogCode&DLGC_WANTCHARS)!=0,L"TWebFrame's custom editor requests character messages from a dialog host");
            std::wstring selectionResult;
            Check(inputView->ExecuteScript(L"const e=document.getElementById('editor');e.setSelectionRange(0,0);return e.selectionStart+'|'+e.selectionEnd;",&selectionResult,&selectionError)&&selectionResult==L"0|0",
                  L"DOM selection APIs control the custom caret");
            inputMessage.clear();SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            Check(inputMessage==L"input:Xabc",L"typing inserts text at the clicked caret and dispatches input");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abc",L"backspace edits the focused text input");
            Check(inputView->ExecuteScript(L"document.getElementById('editor').setSelectionRange(2,2);",nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'Y'),1);
            Check(inputMessage==L"input:abYc",L"typing inserts text in the middle at the custom caret");
            Check(inputView->ExecuteScript(L"document.getElementById('editor').setSelectionRange(2,2);",nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'\uAC00'),1);
            Check(inputMessage==L"input:ab\uAC00Yc",L"custom Unicode editing synchronizes Korean text at the caret");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abYc",L"Korean text can be edited through the same custom caret path");
            SendMessageW(inputWindow,WM_UNDO,0,0);
            Check(inputMessage==L"input:ab\uAC00Yc",L"custom text input undo restores Unicode edits");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abYc",L"editing continues from the caret restored by undo");
            inputMessage.clear();SendMessageW(inputWindow,WM_CHAR,VK_RETURN,1);
            Check(inputMessage.empty(),L"single-line HTML input rejects a custom-editor line break");
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(250,80));
            Check(inputMessage==L"change:abYc",L"leaving an edited text input dispatches change");
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,L"text editing never creates a Win32 Edit child");
            inputMessage.clear();
            Check(inputView->ExecuteScript(L"window.chrome.webview.postMessage(document.getElementById('editor').value);"),L"edited input value is readable through the DOM");
            Check(inputMessage==L"abYc",L"DOM value reflects custom keyboard editing");
            const wchar_t* contenteditableHtml=LR"HTML(<style>*{box-sizing:border-box;margin:0}article{display:block;width:280px;height:90px;padding:10px;font:16px/24px "Segoe UI"}p{display:block;margin:0}</style><article id="rich-editor" contenteditable="true"><p>alpha <strong id="middle-run">bravo</strong> charlie</p></article>)HTML";
            Check(inputView->NavigateToString(contenteditableHtml),L"contenteditable pointer fixture loads in a real view");
            UpdateWindow(inputWindow);
            std::wstring trailingTextPoint;
            Check(inputView->ExecuteScript(
                L"const r=document.getElementById('middle-run').getBoundingClientRect();return Math.round(r.x+r.width+8)+','+Math.round(r.y+r.height/2);",
                &trailingTextPoint,&selectionError),selectionError.c_str());
            const auto trailingPointComma=trailingTextPoint.find(L',');
            if(trailingPointComma!=std::wstring::npos){
                const auto scale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
                const int x=static_cast<int>(std::lround(std::stod(trailingTextPoint.substr(0,trailingPointComma))*scale));
                const int y=static_cast<int>(std::lround(std::stod(trailingTextPoint.substr(trailingPointComma+1))*scale));
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            }
            Check(inputView->ExecuteScript(L"return document.getElementById('rich-editor').textContent;",
                &selectionResult,&selectionError)&&selectionResult.find(L"bravo")<selectionResult.find(L'X')&&
                !selectionResult.empty()&&selectionResult.back()!=L'X',
                L"clicking a later inline text run inserts at its pointed offset instead of a text-node boundary");
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"contenteditable pointer editing remains in the childless common input path");
            const wchar_t* emptyInputsHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}body{font:13px/1.42 "Segoe UI Variable","Segoe UI",sans-serif}input{display:block;width:120px;height:32px;padding:0 4px;border:1px solid #ccc;font:inherit}</style><input type="text"><input type="password"><input type="number">)HTML";
            Check(inputView->NavigateToString(emptyInputsHtml),L"empty input types fixture loads in a real view");
            for(int index=0;index<3;++index){
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,16+32*index));
                SendMessageW(inputWindow,WM_CHAR,index==2?L'1':L'a',1);
            }
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"text, password and number inputs share the childless custom input implementation");
            Check(inputView->ExecuteScript(L"const a=document.querySelectorAll('input');return a[0].value+'|'+a[1].value+'|'+a[2].value;",&selectionResult,&selectionError)&&selectionResult==L"a|a|1",
                  L"custom editing updates every supported input type");
            const wchar_t* fractionalInputHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}input{position:absolute;left:10.5px;top:10.5px;width:120px;height:31px;padding:0;border:1px solid #ccc}</style><input type="text" placeholder="Fractional input">)HTML";
            Check(inputView->NavigateToString(fractionalInputHtml),L"fractional input geometry fixture loads in a real view");
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"fractional CSS geometry remains in the common renderer without an overlay HWND");
            const wchar_t* scrollHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}#scroll{width:200px;height:80px;overflow-y:scroll}.row{height:30px}</style><div id="scroll"><div id="first" class="row">1</div><div class="row">2</div><div class="row">3</div><div class="row">4</div><div class="row">5</div><div id="last" class="row">6</div></div>)HTML";
            Check(inputView->NavigateToString(scrollHtml),L"scrollbar drag fixture loads in a real view");const auto beforeDragLayout=inputView->DumpLayoutJson();
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(192,28));SendMessageW(inputWindow,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(192,65));SendMessageW(inputWindow,WM_LBUTTONUP,0,MAKELPARAM(192,65));const auto afterDragLayout=inputView->DumpLayoutJson();
            Check(beforeDragLayout!=afterDragLayout,L"view mouse capture and thumb dragging update overflowing content layout");
            inputMessage.clear();
            Check(inputView->NavigateToString(L"<style>*{margin:0}#drop-zone{width:200px;height:80px}</style><div id='drop-zone'></div><script>document.getElementById('drop-zone').addEventListener('drop',event=>window.chrome.webview.postMessage(event.dataTransfer.files[0].name))</script>"),
                  L"native file drop fixture loads in a real view");
            wchar_t modulePath[MAX_PATH]{};
            const DWORD modulePathLength=GetModuleFileNameW(nullptr,modulePath,MAX_PATH);
            Check(modulePathLength>0&&modulePathLength<MAX_PATH,L"test executable path is available for native file drop");
            if(modulePathLength>0&&modulePathLength<MAX_PATH){
                const std::wstring dropPath(modulePath,modulePathLength);
                const SIZE_T bytes=sizeof(DROPFILES)+(dropPath.size()+2)*sizeof(wchar_t);
                HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);
                if(memory){auto* header=static_cast<DROPFILES*>(GlobalLock(memory));
                    header->pFiles=sizeof(DROPFILES);header->pt={10,10};header->fWide=TRUE;
                    auto* name=reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(header)+sizeof(DROPFILES));
                    std::copy(dropPath.begin(),dropPath.end(),name);GlobalUnlock(memory);
                    SendMessageW(inputWindow,WM_DROPFILES,reinterpret_cast<WPARAM>(memory),0);
                }
                Check(inputMessage==L"TWebFrameTests.exe",L"native file drop reaches the DOM drop event at the pointer location");
            }
            std::vector<std::wstring> frameEvents;
            inputView->SetMessageHandler([&](const std::wstring& value){frameEvents.push_back(value);});
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& source){
                if(path!=L"child.html")return false;
                source=L"<script>window.addEventListener('message',event=>{if(event.source===parent&&event.data.kind==='reply')parent.postMessage({kind:'ack'},'*');});parent.postMessage({kind:'ready'},'*');</script>";
                return true;
            });
            const wchar_t* frameHtml=LR"HTML(<style>*{margin:0}iframe{width:150px;height:80px;border:0}</style><iframe id="child" src="child.html"></iframe><script>const frame=document.getElementById('child');window.addEventListener('message',event=>{if(event.source!==frame.contentWindow)return;if(event.data.kind==='ready')frame.contentWindow.postMessage({kind:'reply'},'*');if(event.data.kind==='ack')window.chrome.webview.postMessage('iframe-ok');});frame.addEventListener('load',()=>window.chrome.webview.postMessage('iframe-loaded'));</script>)HTML";
            Check(inputView->NavigateToString(frameHtml),inputView->LastError().c_str());
            Check(frameEvents.size()==2&&frameEvents[0]==L"iframe-ok"&&frameEvents[1]==L"iframe-loaded",
                  L"iframe documents exchange structured messages with parent and dispatch load");
            Check(FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr)!=nullptr,
                  L"iframe has a separately rendered TWebFrame child window");
        }
        DestroyWindow(inputHost);
    }

    if(uninitializeCom)CoUninitialize();
    if(failures){std::wcerr<<failures<<L" test(s) failed\n";return 1;}
    std::wcout << L"All TWebFrame tests passed\n";return 0;
}
