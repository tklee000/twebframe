#include <TWebFrame/TWebFrame.h>

#include "Accessibility.h"
#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"
#include "TextInput.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace TWebFrame {
using Microsoft::WRL::ComPtr;
using namespace Internal;

namespace {
constexpr wchar_t kWindowClass[] = L"TWebFrame.View.1";
constexpr UINT_PTR kAnimationFrameTimer = 0x5746;
constexpr UINT_PTR kJavaScriptTimer = 0x5747;
constexpr UINT_PTR kCssTransitionTimer = 0x5748;
constexpr UINT_PTR kCaretBlinkTimer = 0x5749;
constexpr UINT kAnimationFrameFallbackMessage = WM_APP + 0x57;

std::wstring Utf8ToWide(const std::string& value) {
    if(value.empty())return {};int count=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);std::wstring out(count,L'\0');MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),count);return out;
}

ATOM EnsureWindowClass(HINSTANCE instance,const wchar_t* className,WNDPROC proc) {
    WNDCLASSEXW existing{};if(GetClassInfoExW(instance,className,&existing))return 1;
    // WM_SIZE explicitly invalidates the view. Avoid the class-wide redraw
    // flags, which add redundant full-window invalidations during live resize.
    WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_DBLCLKS;wc.lpfnWndProc=proc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;wc.lpszClassName=className;return RegisterClassExW(&wc);
}

bool IsTextInput(const std::shared_ptr<Node>& node) {
    if(!node||node->tag!=L"input")return false;
    const auto type=ToLower(node->Attribute(L"type"));
    return type.empty()||type==L"text"||type==L"search"||type==L"tel"||
           type==L"url"||type==L"email"||type==L"password"||type==L"number";
}

bool IsTextControl(const std::shared_ptr<Node>& node) {
    return IsTextInput(node)||(node&&node->tag==L"textarea");
}

bool IsEditableTextControl(const std::shared_ptr<Node>& node) {
    return IsTextControl(node)&&!node->disabled&&!node->attributes.count(L"readonly");
}

bool IsContentEditable(const std::shared_ptr<Node>& node) {
    if(!node||!node->attributes.count(L"contenteditable"))return false;
    const auto value=ToLower(Trim(node->Attribute(L"contenteditable")));
    return value.empty()||value==L"true"||value==L"plaintext-only";
}

std::shared_ptr<Node> EditableRoot(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock()){
        if(current->attributes.count(L"contenteditable"))
            return IsContentEditable(current)?current:std::shared_ptr<Node>{};
    }
    return {};
}

bool IsFocusable(const std::shared_ptr<Node>& node) {
    if(!node||node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true")return false;
    if(node->attributes.count(L"tabindex"))return true;
    if(IsContentEditable(node))return true;
    if(node->tag==L"input"||node->tag==L"button"||node->tag==L"select"||node->tag==L"textarea")return true;
    if(node->tag==L"a"&&!node->Attribute(L"href").empty())return true;
    const auto role=ToLower(node->Attribute(L"role"));
    return role==L"button"||role==L"menuitem"||role==L"checkbox"||role==L"radio"||role==L"switch"||role==L"tab"||role==L"option";
}

int SequentialTabIndex(const std::shared_ptr<Node>& node){
    if(!IsFocusable(node))return -1;
    if(!node->attributes.count(L"tabindex"))return 0;
    try{return std::stoi(node->Attribute(L"tabindex"));}catch(...){return 0;}
}

bool IsKeyboardActivatable(const std::shared_ptr<Node>& node){
    if(!node||node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true")return false;
    if(node->tag==L"button"||node->tag==L"a")return true;
    if(node->tag==L"input"){
        const auto type=ToLower(node->Attribute(L"type"));
        return type==L"button"||type==L"submit"||type==L"reset"||type==L"checkbox"||
               type==L"radio"||type==L"file"||type==L"image";
    }
    const auto role=ToLower(node->Attribute(L"role"));
    return role==L"button"||role==L"menuitem"||role==L"checkbox"||role==L"radio"||
           role==L"switch"||role==L"tab"||role==L"option";
}

bool FileInfoForPath(const std::wstring& path,Node::FileInfo& result){
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&metadata))return false;
    const bool directory=(metadata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const std::filesystem::path selected(path);
    const auto extension=ToLower(selected.extension().wstring());
    result.name=selected.filename().wstring();
    if(result.name.empty())result.name=selected.root_name().wstring();
    result.path=selected.lexically_normal().wstring();
    result.type=directory?L"application/x-directory":extension==L".csv"?L"text/csv":extension==L".json"?L"application/json":
        extension==L".txt"||extension==L".log"?L"text/plain":L"application/octet-stream";
    result.size=directory?0:(static_cast<unsigned long long>(metadata.nFileSizeHigh)<<32)|metadata.nFileSizeLow;
    return true;
}

std::vector<std::shared_ptr<Node>> SelectOptions(const std::shared_ptr<Node>& select) {
    std::vector<std::shared_ptr<Node>> result;
    if(!select||select->tag!=L"select")return result;
    std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
        for(const auto& child:parent->children){
            if(child->tag==L"option")result.push_back(child);
            else if(child->tag==L"optgroup")collect(child);
        }
    };
    collect(select);return result;
}

std::wstring OptionValue(const std::shared_ptr<Node>& option) {
    return option&&option->attributes.count(L"value")?option->Attribute(L"value"):
           (option?option->InnerText():L"");
}

std::wstring KeyValue(WPARAM key) {
    switch(key){
    case VK_ESCAPE:return L"Escape";case VK_RETURN:return L"Enter";case VK_TAB:return L"Tab";
    case VK_BACK:return L"Backspace";case VK_DELETE:return L"Delete";case VK_SPACE:return L" ";
    case VK_LEFT:return L"ArrowLeft";case VK_RIGHT:return L"ArrowRight";
    case VK_UP:return L"ArrowUp";case VK_DOWN:return L"ArrowDown";
    case VK_HOME:return L"Home";case VK_END:return L"End";case VK_PRIOR:return L"PageUp";case VK_NEXT:return L"PageDown";
    default:break;
    }
    if(key>=L'A'&&key<=L'Z')return std::wstring(1,static_cast<wchar_t>(((GetKeyState(VK_SHIFT)&0x8000)!=0)?key:key-L'A'+L'a'));
    if(key>=L'0'&&key<=L'9')return std::wstring(1,static_cast<wchar_t>(key));
    return L"Unidentified";
}

size_t SelectedOptionIndex(const std::shared_ptr<Node>& select,
                           const std::vector<std::shared_ptr<Node>>& options) {
    if(options.empty())return 0;
    if(select&&select->attributes.count(L"value")){
        const auto value=select->Attribute(L"value");
        for(size_t index=0;index<options.size();++index)
            if(OptionValue(options[index])==value)return index;
    }
    for(size_t index=0;index<options.size();++index)
        if(options[index]->attributes.count(L"selected"))return index;
    return 0;
}

size_t PreviousTextPosition(const std::wstring& value,size_t position) {
    if(position==0)return 0;--position;
    if(position>0&&value[position]>=0xdc00&&value[position]<=0xdfff&&
       value[position-1]>=0xd800&&value[position-1]<=0xdbff)--position;
    return position;
}

size_t NextTextPosition(const std::wstring& value,size_t position) {
    if(position>=value.size())return value.size();
    if(value[position]>=0xd800&&value[position]<=0xdbff&&position+1<value.size()&&
       value[position+1]>=0xdc00&&value[position+1]<=0xdfff)return position+2;
    return position+1;
}

unsigned int CompositeOver(unsigned int foreground,unsigned int background) {
    const unsigned int alpha=(foreground>>24)&0xff;
    if(alpha==0)return background;
    if(alpha==255)return 0xff000000u|(foreground&0x00ffffffu);
    auto channel=[&](int shift){
        const unsigned int front=(foreground>>shift)&0xff,back=(background>>shift)&0xff;
        return (front*alpha+back*(255-alpha)+127)/255;
    };
    return 0xff000000u|(channel(16)<<16)|(channel(8)<<8)|channel(0);
}

unsigned int EffectiveBackgroundColor(const LayoutBox* box) {
    std::vector<const LayoutBox*> ancestors;
    for(auto* current=box;current;current=current->parent)ancestors.push_back(current);
    // This is also the clear color used for a TWebFrame document.
    unsigned int result=0xfff3f5f8u;
    constexpr unsigned int invalid=0x01020304u;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        auto value=(*it)->style.Get(L"background-color");
        if(value.empty())value=(*it)->style.Get(L"background");
        if(value.empty())continue;
        if(ToLower(Trim(value))==L"currentcolor")value=(*it)->style.Get(L"color",L"#000000");
        const auto color=StyleSheet::Color(value,invalid);
        if(color!=invalid)result=CompositeOver(color,result);
    }
    return result;
}
}

struct View::Impl {
    HWND hwnd=nullptr;
    Document document;
    StyleSheet styles;
    JavaScriptRuntime javascript{document};
    LayoutEngine layout{document,styles};
    MessageHandler messageHandler;
    LoadHandler loadHandler;
    View::ResourceLoader resourceLoader;
    struct ChildFrame { std::shared_ptr<Node> node; std::unique_ptr<View> view; };
    std::vector<ChildFrame> childFrames;
    std::wstring lastError;
    std::wstring basePath;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
    ComPtr<ID2D1BitmapRenderTarget> backBuffer;
    D2D1_SIZE_U backBufferPixelSize{};
    float backBufferDpi=0;
    std::shared_ptr<AccessibilityHost> accessibility;
    std::vector<std::weak_ptr<Node>> liveRegions;
    std::unordered_map<const Node*,std::wstring> liveRegionText;
    struct AccessibilityPosition {
        std::weak_ptr<Node> parent;
        const Node* parentKey = nullptr;
        size_t index = 0;
    };
    std::unordered_map<const Node*,std::vector<std::shared_ptr<Node>>> accessibilityChildrenCache;
    std::unordered_map<const Node*,AccessibilityPosition> accessibilityPositionCache;
    std::unordered_map<const Node*,std::wstring> accessibilityAutomationIdCache;
    bool accessibilityTreeDirty = true;
    std::shared_ptr<Node> focused;
    std::shared_ptr<Node> hovered;
    std::vector<std::shared_ptr<Node>> hoverPath;
    std::shared_ptr<Node> scrollbarDragNode;
    float scrollbarDragOffset=0;
    bool scrollbarDragHorizontal=false;
    std::shared_ptr<Node> openSelectPopup;
    int selectPopupHotIndex=-1;
    float selectPopupScrollOffset=0;
    bool selectPopupShowAll=false;
    bool selectPopupScrollDragging=false;
    float selectPopupScrollDragOffset=0;
    std::shared_ptr<Node> editingNode;
    bool textSelectionDragging=false;
    size_t selectionAnchor=0;
    size_t caretPosition=0;
    bool caretVisible=true;
    bool caretBlinkTimerActive=false;
    bool textEditDirty=false;
    bool compositionActive=false;
    std::wstring compositionBase;
    std::wstring compositionText;
    std::vector<unsigned char> compositionAttributes;
    size_t compositionReplaceStart=0;
    size_t compositionReplaceEnd=0;
    size_t compositionCursor=0;
    struct EditSnapshot {
        std::wstring value;
        size_t anchor=0;
        size_t caret=0;
    };
    std::vector<EditSnapshot> undoHistory;
    std::vector<EditSnapshot> redoHistory;
    bool layoutDirty=true;
    bool viewportOnlyDirty=false;
    bool trackingMouseLeave=false;
    bool cssTransitionTimerActive=false;
    std::chrono::steady_clock::time_point cssTransitionTick{};
    TextInput textInput;

    Impl():textInput(TextInput::Client{
        [this]{return CanEditText();},
        [this]{BeginTextComposition();},
        [this](const std::wstring& text,const std::vector<unsigned char>& attributes,size_t cursor){
            UpdateTextComposition(text,attributes,cursor);
        },
        [this](const std::wstring& text){CommitTextComposition(text);},
        [this]{CancelTextComposition();},
        [this]{return CaretClientRect();}
    }){}

    bool LoadTextResource(const std::wstring& reference,std::wstring& content)const{
        auto resource=reference;
        const auto suffix=resource.find_first_of(L"?#");
        if(suffix!=std::wstring::npos)resource.resize(suffix);
        auto resolved=resource;const auto scheme=basePath.find(L"://");
        if(scheme!=std::wstring::npos&&resource.find(L"://")==std::wstring::npos){
            auto base=basePath;const auto baseSuffix=base.find_first_of(L"?#");if(baseSuffix!=std::wstring::npos)base.resize(baseSuffix);
            const auto originEnd=base.find(L'/',scheme+3);
            if(resource.rfind(L"//",0)==0)resolved=base.substr(0,scheme+1)+resource;
            else if(!resource.empty()&&resource.front()==L'/')resolved=(originEnd==std::wstring::npos?base:base.substr(0,originEnd))+resource;
            else{const auto slash=base.find_last_of(L'/');resolved=(slash==std::wstring::npos?base+L'/':base.substr(0,slash+1))+resource;}
        }
        if(resourceLoader&&resourceLoader(resolved,content))return true;
        if(resolved!=resource&&resourceLoader&&resourceLoader(resource,content))return true;
        if(scheme!=std::wstring::npos)return false;
        std::filesystem::path path(resource);
        if(path.is_relative()&&!basePath.empty())path=std::filesystem::path(basePath)/path;
        std::ifstream input(path,std::ios::binary);if(!input)return false;
        std::ostringstream bytes;bytes<<input.rdbuf();content=Utf8ToWide(bytes.str());return true;
    }

    float DpiScale()const{
        const UINT dpi=hwnd?GetDpiForWindow(hwnd):USER_DEFAULT_SCREEN_DPI;
        return std::max(1.0f,static_cast<float>(dpi)/static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    }
    float PixelToDip(float value)const{return value/DpiScale();}
    LONG DipToPixel(float value)const{return static_cast<LONG>(std::lround(value*DpiScale()));}
    static void IncludeBounds(LayoutRect& bounds,bool& initialized,const LayoutRect& item){
        if(item.width<=0||item.height<=0)return;
        if(!initialized){bounds=item;initialized=true;return;}
        const float left=std::min(bounds.x,item.x),top=std::min(bounds.y,item.y);
        const float right=std::max(bounds.x+bounds.width,item.x+item.width);
        const float bottom=std::max(bounds.y+bounds.height,item.y+item.height);
        bounds={left,top,right-left,bottom-top};
    }
    void InvalidateDipBounds(const LayoutRect& bounds){
        const float scale=DpiScale();
        RECT dirty{static_cast<LONG>(std::floor(bounds.x*scale)),
            static_cast<LONG>(std::floor(bounds.y*scale)),
            static_cast<LONG>(std::ceil((bounds.x+bounds.width)*scale)),
            static_cast<LONG>(std::ceil((bounds.y+bounds.height)*scale))};
        InflateRect(&dirty,1,1);RECT client{};GetClientRect(hwnd,&client);
        RECT clipped{};if(IntersectRect(&clipped,&dirty,&client))InvalidateRect(hwnd,&clipped,FALSE);
    }
    void UpdateJavaScriptViewport(){
        RECT bounds{};GetClientRect(hwnd,&bounds);const float scale=DpiScale();
        javascript.SetViewportSize(static_cast<double>(std::max(1L,bounds.right-bounds.left))/scale,
                                   static_cast<double>(std::max(1L,bounds.bottom-bounds.top))/scale);
        javascript.SetDevicePixelRatio(scale);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(lParam);self=static_cast<Impl*>(cs->lpCreateParams);self->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        return self?self->HandleMessage(message,wParam,lParam):DefWindowProcW(hwnd,message,wParam,lParam);
    }
    bool Initialize(HWND parent,const RECT& bounds){
        auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent,GWLP_HINSTANCE));if(!instance)instance=GetModuleHandleW(nullptr);if(!EnsureWindowClass(instance,kWindowClass,&Impl::WindowProc)){lastError=L"Failed to register TWebFrame window class";return false;}
        hwnd=CreateWindowExW(0,kWindowClass,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,parent,nullptr,instance,this);if(!hwnd){lastError=L"Failed to create TWebFrame child window";return false;}
        DragAcceptFiles(hwnd,TRUE);
        HRESULT hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,d2dFactory.ReleaseAndGetAddressOf());if(FAILED(hr)){lastError=L"D2D1CreateFactory failed";return false;}
        hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf()));if(FAILED(hr)){lastError=L"DWriteCreateFactory failed";return false;}
        javascript.SetMessageSink([this](const std::wstring& msg){if(messageHandler)messageHandler(msg);});
        javascript.SetMutationSink([this](const JavaScriptRuntime::Mutation& mutation){HandleMutation(mutation);});
        javascript.SetFocusSink([this](const std::shared_ptr<Node>& node){
            SetFocusedNode(node,true,true);
        });
        javascript.SetSelectionProvider([this](const std::shared_ptr<Node>& node,size_t& start,size_t& end){
            if(!IsTextControl(node))return false;
            if(node==focused&&editingNode==node){start=std::min(selectionAnchor,caretPosition);end=std::max(selectionAnchor,caretPosition);}
            else{start=node->selectionStart;end=node->selectionEnd;}
            return true;
        });
        javascript.SetSelectionSetter([this](const std::shared_ptr<Node>& node,size_t start,size_t end){
            if(node!=focused||!IsTextControl(node))return;editingNode=node;
            const auto length=node->Attribute(L"value").size();selectionAnchor=std::min(start,length);
            caretPosition=std::min(end,length);ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);
        });
        javascript.SetFrameScheduler([this]{
            if(!SetTimer(hwnd,kAnimationFrameTimer,16,nullptr))
                PostMessageW(hwnd,kAnimationFrameFallbackMessage,0,0);
        });
        javascript.SetTimerScheduler([this](unsigned delay){
            KillTimer(hwnd,kJavaScriptTimer);
            if(delay)SetTimer(hwnd,kJavaScriptTimer,std::max(1u,delay),nullptr);
        });
        javascript.SetResourceLoader([this](const std::wstring& resource,std::wstring& content){
            return LoadTextResource(resource,content);
        });
        javascript.SetDialogSink([this](const std::wstring& message){
            MessageBoxW(hwnd,message.c_str(),L"",MB_OK|MB_ICONINFORMATION);
        });
        javascript.SetFrameMessageSink([this](const std::shared_ptr<Node>& node,const std::wstring& data){
            for(auto& frame:childFrames)if(frame.node==node){
                frame.view->impl_->javascript.DispatchWindowMessageAsJson(data);
                frame.view->impl_->layoutDirty=true;
                InvalidateRect(frame.view->Window(),nullptr,FALSE);
                break;
            }
        });
        javascript.SetGeometryProvider([this](const std::shared_ptr<Node>& node){
            JavaScriptRuntime::NodeGeometry geometry;
            if(layoutDirty)Rebuild();
            if(const auto* box=layout.BoxFor(node)){
                geometry.x=box->rect.x;geometry.y=box->rect.y;
                geometry.width=box->rect.width;geometry.height=box->rect.height;
                geometry.clientWidth=box->content.width;geometry.clientHeight=box->content.height;
                geometry.scrollWidth=std::max(box->content.width,box->scrollWidth);
                geometry.scrollHeight=std::max(box->content.height,box->scrollHeight);
            }
            return geometry;
        });
        javascript.SetStylePropertyProvider([this](const std::shared_ptr<Node>& node,const std::wstring& property){
            if(layoutDirty)Rebuild();if(const auto* box=layout.BoxFor(node))return box->style.Get(property);return styles.Compute(node).Get(property);
        });
        InitializeAccessibility();
        return true;
    }
    void ResetRenderTargets(){backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;renderTarget.Reset();}
    void EnsureTarget(){
        if(renderTarget||!d2dFactory)return;
        backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;
        RECT r{};GetClientRect(hwnd,&r);const auto size=D2D1::SizeU(
            static_cast<UINT32>(std::max(1L,r.right-r.left)),
            static_cast<UINT32>(std::max(1L,r.bottom-r.top)));
        const auto properties=D2D1::HwndRenderTargetProperties(hwnd,size);
        if(SUCCEEDED(d2dFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),properties,
            renderTarget.ReleaseAndGetAddressOf()))){const float dpi=USER_DEFAULT_SCREEN_DPI*DpiScale();renderTarget->SetDpi(dpi,dpi);}
    }
    bool EnsureBackBuffer(D2D1_SIZE_U required,float dpi){
        if(!renderTarget)return false;
        const bool dpiChanged=std::abs(backBufferDpi-dpi)>0.01f;
        const bool tooSmall=backBufferPixelSize.width<required.width||backBufferPixelSize.height<required.height;
        const bool excessivelyLarge=backBufferPixelSize.width>required.width*2u||backBufferPixelSize.height>required.height*2u;
        if(backBuffer&&!dpiChanged&&!tooSmall&&!excessivelyLarge)return true;
        D2D1_SIZE_U capacity=required;
        if(backBuffer&&!dpiChanged&&tooSmall){
            capacity.width=std::max(required.width,backBufferPixelSize.width+std::max(64u,backBufferPixelSize.width/4u));
            capacity.height=std::max(required.height,backBufferPixelSize.height+std::max(64u,backBufferPixelSize.height/4u));
        }
        backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;
        const auto dipSize=D2D1::SizeF(capacity.width*USER_DEFAULT_SCREEN_DPI/dpi,
            capacity.height*USER_DEFAULT_SCREEN_DPI/dpi);
        if(FAILED(renderTarget->CreateCompatibleRenderTarget(&dipSize,&capacity,nullptr,
            D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,backBuffer.ReleaseAndGetAddressOf())))return false;
        backBuffer->SetDpi(dpi,dpi);backBufferPixelSize=capacity;backBufferDpi=dpi;return true;
    }
    void UpdateFrameBounds(){
        const float scale=DpiScale();
        for(auto& frame:childFrames){
            const auto* box=layout.BoxFor(frame.node);
            if(!box||!box->visible||box->content.width<=0||box->content.height<=0){frame.view->SetVisible(false);continue;}
            RECT bounds{static_cast<LONG>(std::floor(box->content.x*scale)),
                static_cast<LONG>(std::floor(box->content.y*scale)),
                static_cast<LONG>(std::ceil((box->content.x+box->content.width)*scale)),
                static_cast<LONG>(std::ceil((box->content.y+box->content.height)*scale))};
            frame.view->SetBounds(bounds);
            frame.view->SetVisible(true);
        }
    }
    void SyncCssTransitionTimer(){
        if(layout.HasActiveTransitions()){
            if(!cssTransitionTimerActive){
                cssTransitionTick=std::chrono::steady_clock::now();
                cssTransitionTimerActive=SetTimer(hwnd,kCssTransitionTimer,16,nullptr)!=0;
            }
        }else if(cssTransitionTimerActive){
            KillTimer(hwnd,kCssTransitionTimer);cssTransitionTimerActive=false;
        }
    }
    void Rebuild(){RECT r{};GetClientRect(hwnd,&r);const float scale=DpiScale();const float width=static_cast<float>(std::max(1L,r.right))/scale,height=static_cast<float>(std::max(1L,r.bottom))/scale;javascript.SetViewportSize(width,height);if(viewportOnlyDirty)layout.Relayout(width,height,scale);else layout.Layout(width,height,scale);layoutDirty=false;viewportOnlyDirty=false;accessibilityTreeDirty=true;if(accessibility)accessibility->Invalidate();UpdateFrameBounds();SyncCssTransitionTimer();}
    bool AccessibilityHidden(const std::shared_ptr<Node>& node)const{
        for(auto current=node;current;current=current->parent.lock())
            if(ToLower(current->Attribute(L"aria-hidden"))==L"true"||current->attributes.count(L"hidden"))return true;
        return false;
    }
    bool IsAccessibilityNode(const std::shared_ptr<Node>& node){
        if(!node||node->type==NodeType::Document||AccessibilityHidden(node))return false;
        if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);
        if(node->tag==L"option"){
            const auto container=OwningSelect(node);const auto* selectBox=container?layout.BoxFor(container):nullptr;
            return selectBox&&selectBox->visible;
        }
        if(!box||!box->visible)return false;
        if(node->type==NodeType::Text){
            if(Trim(node->text).empty())return false;const auto parent=node->parent.lock();
            return !parent||!(IsFocusable(parent)||parent->tag==L"button"||parent->tag==L"label"||
                parent->tag==L"option"||!parent->Attribute(L"aria-label").empty());
        }
        const auto role=ToLower(node->Attribute(L"role"));
        if(role==L"none"||role==L"presentation")return IsFocusable(node);
        if(IsFocusable(node)||!role.empty()||!node->Attribute(L"aria-label").empty()||
           !node->Attribute(L"aria-labelledby").empty()||!node->Attribute(L"aria-live").empty())return true;
        const auto& tag=node->tag;
        return tag==L"h1"||tag==L"h2"||tag==L"h3"||tag==L"h4"||tag==L"h5"||tag==L"h6"||
            tag==L"img"||tag==L"ul"||tag==L"ol"||tag==L"li"||tag==L"table"||tag==L"tr"||
            tag==L"th"||tag==L"td"||tag==L"nav"||tag==L"main"||tag==L"section"||tag==L"aside";
    }
    void EnsureAccessibilityTree(){
        if(!accessibilityTreeDirty)return;
        if(layoutDirty)Rebuild();
        accessibilityChildrenCache.clear();accessibilityPositionCache.clear();
        accessibilityAutomationIdCache.clear();
        std::function<void(const std::shared_ptr<Node>&,const std::wstring&)> cacheAutomationIds=
            [&](const std::shared_ptr<Node>& parent,const std::wstring& parentPath){
                std::unordered_map<std::wstring,size_t> indexes;
                for(const auto& child:parent->children){
                    if(!child)continue;
                    const auto name=child->type==NodeType::Text?L"text":
                        (child->tag.empty()?L"node":child->tag);
                    const auto index=++indexes[name];
                    const auto path=parentPath.empty()?name+L"["+std::to_wstring(index)+L"]":
                        parentPath+L"/"+name+L"["+std::to_wstring(index)+L"]";
                    std::wstring automationId;
                    for(const auto* attribute:{L"data-automation-id",L"id",L"name"}){
                        automationId=child->Attribute(attribute);if(!automationId.empty())break;
                    }
                    accessibilityAutomationIdCache.emplace(
                        child.get(),automationId.empty()?path:std::move(automationId));
                    cacheAutomationIds(child,path);
                }
            };
        const auto root=document.Root();if(root)cacheAutomationIds(root,L"");
        std::function<void(const std::shared_ptr<Node>&,const std::shared_ptr<Node>&)> collect=
            [&](const std::shared_ptr<Node>& current,const std::shared_ptr<Node>& accessibleParent){
                if(!current||current->attributes.count(L"hidden")||
                   ToLower(current->Attribute(L"aria-hidden"))==L"true")return;
                auto parent=accessibleParent;
                if(IsAccessibilityNode(current)){
                    auto& siblings=accessibilityChildrenCache[parent.get()];
                    accessibilityPositionCache[current.get()]={parent,parent.get(),siblings.size()};
                    siblings.push_back(current);parent=current;
                }
                for(const auto& child:current->children)collect(child,parent);
            };
        if(root)for(const auto& child:root->children)collect(child,{});
        accessibilityTreeDirty=false;
    }
    std::vector<std::shared_ptr<Node>> AccessibilityChildren(const std::shared_ptr<Node>& parent){
        EnsureAccessibilityTree();const auto found=accessibilityChildrenCache.find(parent.get());
        return found==accessibilityChildrenCache.end()?std::vector<std::shared_ptr<Node>>{}:found->second;
    }
    std::shared_ptr<Node> AccessibilityParent(const std::shared_ptr<Node>& node){
        EnsureAccessibilityTree();const auto found=accessibilityPositionCache.find(node.get());
        return found==accessibilityPositionCache.end()?std::shared_ptr<Node>{}:found->second.parent.lock();
    }
    std::shared_ptr<Node> AccessibilitySibling(const std::shared_ptr<Node>& node,bool next){
        EnsureAccessibilityTree();const auto found=accessibilityPositionCache.find(node.get());
        if(found==accessibilityPositionCache.end())return {};
        const auto siblings=accessibilityChildrenCache.find(found->second.parentKey);
        if(siblings==accessibilityChildrenCache.end())return {};
        const auto index=found->second.index;
        if(next)return index+1<siblings->second.size()?siblings->second[index+1]:std::shared_ptr<Node>{};
        return index>0&&index<=siblings->second.size()?siblings->second[index-1]:std::shared_ptr<Node>{};
    }
    std::wstring AccessibilityName(const std::shared_ptr<Node>& node){
        if(!node)return L"TWebFrame";if(node->type==NodeType::Text)return Trim(node->text);
        auto label=node->Attribute(L"aria-label");if(!label.empty())return label;
        const auto labelledBy=node->Attribute(L"aria-labelledby");
        if(!labelledBy.empty()){
            std::wistringstream ids(labelledBy);std::wstring id,name;
            while(ids>>id)if(auto source=document.GetElementById(id)){if(!name.empty())name+=L" ";name+=Trim(source->InnerText());}
            if(!name.empty())return name;
        }
        const auto id=node->Attribute(L"id");
        if(!id.empty())for(const auto& candidate:document.QuerySelectorAll(L"label"))
            if(candidate->Attribute(L"for")==id&&!Trim(candidate->InnerText()).empty())return Trim(candidate->InnerText());
        if(node->tag==L"img"&&!node->Attribute(L"alt").empty())return node->Attribute(L"alt");
        if(!node->Attribute(L"title").empty())return node->Attribute(L"title");
        const auto type=ToLower(node->Attribute(L"type"));
        if(node->tag==L"input"&&(type==L"button"||type==L"submit"||type==L"reset"||type==L"file")&&!node->Attribute(L"value").empty())return node->Attribute(L"value");
        if(IsTextInput(node)&&!node->Attribute(L"placeholder").empty())return node->Attribute(L"placeholder");
        if(node->tag==L"select"){
            const auto options=SelectOptions(node);if(!options.empty())return Trim(options[SelectedOptionIndex(node,options)]->InnerText());
        }
        const auto role=ToLower(node->Attribute(L"role"));
        const bool roleNamesFromContent=role==L"button"||role==L"menuitem"||role==L"option"||
            role==L"tab"||role==L"checkbox"||role==L"radio"||role==L"switch"||
            role==L"status"||role==L"alert"||role==L"link"||role==L"heading"||
            role==L"treeitem"||role==L"cell"||role==L"columnheader"||role==L"rowheader";
        const auto& tag=node->tag;
        const bool tagNamesFromContent=tag==L"button"||tag==L"a"||tag==L"label"||
            tag==L"option"||tag==L"li"||tag==L"th"||tag==L"td"||tag==L"summary"||
            tag==L"legend"||(tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6');
        return roleNamesFromContent||tagNamesFromContent?Trim(node->InnerText()):L"";
    }
    std::wstring StableAutomationId(const std::shared_ptr<Node>& node)const{
        if(!node)return L"twebframe-root";
        const auto cached=accessibilityAutomationIdCache.find(node.get());
        if(cached!=accessibilityAutomationIdCache.end())return cached->second;
        for(const auto* attribute:{L"data-automation-id",L"id",L"name"}){
            const auto value=node->Attribute(attribute);if(!value.empty())return value;
        }
        std::vector<std::wstring> parts;
        for(auto current=node;current&&current!=document.Root();current=current->parent.lock()){
            const auto parent=current->parent.lock();size_t index=1;
            if(parent)for(const auto& sibling:parent->children){
                if(sibling==current)break;if(sibling->type==current->type&&sibling->tag==current->tag)++index;
            }
            const auto name=current->type==NodeType::Text?L"text":(current->tag.empty()?L"node":current->tag);
            parts.push_back(name+L"["+std::to_wstring(index)+L"]");
        }
        std::wstring result;
        for(auto it=parts.rbegin();it!=parts.rend();++it){if(!result.empty())result+=L"/";result+=*it;}
        return result.empty()?L"twebframe-element":result;
    }
    std::shared_ptr<Node> OwningSelect(const std::shared_ptr<Node>& node)const{
        auto current=node;
        while(current&&current->tag!=L"select")current=current->parent.lock();return current;
    }
    AccessibilityNodeInfo AccessibilityInfo(const std::shared_ptr<Node>& node){
        AccessibilityNodeInfo info;info.root=!node;info.valid=info.root||IsAccessibilityNode(node);if(!info.valid)return info;
        if(layoutDirty)Rebuild();
        if(info.root){
            info.controlType=UIA_PaneControlTypeId;info.name=L"TWebFrame";info.automationId=L"twebframe-root";
            info.className=L"TWebFrame.View";info.focusable=true;info.focused=GetFocus()==hwnd||IsChild(hwnd,GetFocus());
            RECT rect{};GetClientRect(hwnd,&rect);POINT origin{0,0};ClientToScreen(hwnd,&origin);
            info.bounds={static_cast<double>(origin.x),static_cast<double>(origin.y),
                         static_cast<double>(rect.right),static_cast<double>(rect.bottom)};return info;
        }
        EnsureAccessibilityTree();
        info.name=AccessibilityName(node);info.automationId=StableAutomationId(node);
        info.className=node->type==NodeType::Text?L"#text":node->tag;info.focusable=IsFocusable(node);
        info.focused=node->focused;info.enabled=!node->disabled&&ToLower(node->Attribute(L"aria-disabled"))!=L"true";
        info.readOnly=node->attributes.count(L"readonly")!=0||ToLower(node->Attribute(L"aria-readonly"))==L"true";info.password=ToLower(node->Attribute(L"type"))==L"password";
        info.helpText=node->Attribute(L"aria-description");if(info.helpText.empty())info.helpText=node->Attribute(L"title");
        info.ariaRole=ToLower(node->Attribute(L"role"));const auto role=info.ariaRole;
        const auto checked=ToLower(node->Attribute(L"aria-checked"));
        info.mixed=node->indeterminate||checked==L"mixed";info.checked=node->checked||checked==L"true";
        info.expanded=IsDatalistInput(node)?openSelectPopup==node:ToLower(node->Attribute(L"aria-expanded"))==L"true";
        const auto live=ToLower(node->Attribute(L"aria-live"));info.liveSetting=live==L"assertive"?Assertive:live==L"polite"?Polite:Off;
        std::vector<std::wstring> aria;
        if(node->attributes.count(L"aria-checked"))aria.push_back(L"checked="+checked);
        if(node->disabled||node->attributes.count(L"aria-disabled"))aria.push_back(L"disabled="+std::wstring(info.enabled?L"false":L"true"));
        if(node->attributes.count(L"aria-expanded"))aria.push_back(L"expanded="+std::wstring(info.expanded?L"true":L"false"));
        if(!live.empty())aria.push_back(L"live="+live);
        for(const auto& item:aria){if(!info.ariaProperties.empty())info.ariaProperties+=L";";info.ariaProperties+=item;}
        const auto type=ToLower(node->Attribute(L"type"));
        if(role==L"button")info.controlType=UIA_ButtonControlTypeId;
        else if(role==L"menu")info.controlType=UIA_MenuControlTypeId;
        else if(role==L"menubar")info.controlType=UIA_MenuBarControlTypeId;
        else if(role==L"menuitem")info.controlType=UIA_MenuItemControlTypeId;
        else if(role==L"checkbox"||role==L"switch")info.controlType=UIA_CheckBoxControlTypeId;
        else if(role==L"radio")info.controlType=UIA_RadioButtonControlTypeId;
        else if(role==L"tab")info.controlType=UIA_TabItemControlTypeId;
        else if(role==L"tablist")info.controlType=UIA_TabControlTypeId;
        else if(role==L"option")info.controlType=UIA_ListItemControlTypeId;
        else if(role==L"listbox")info.controlType=UIA_ListControlTypeId;
        else if(role==L"textbox")info.controlType=UIA_EditControlTypeId;
        else if(role==L"status"||role==L"alert")info.controlType=UIA_TextControlTypeId;
        else if(node->tag==L"button")info.controlType=UIA_ButtonControlTypeId;
        else if(node->tag==L"a")info.controlType=UIA_HyperlinkControlTypeId;
        else if(node->tag==L"select"||IsDatalistInput(node))info.controlType=UIA_ComboBoxControlTypeId;
        else if(node->tag==L"option")info.controlType=UIA_ListItemControlTypeId;
        else if(node->tag==L"input"&&(type==L"checkbox"||type==L"radio"))info.controlType=type==L"radio"?UIA_RadioButtonControlTypeId:UIA_CheckBoxControlTypeId;
        else if(IsTextInput(node)||node->tag==L"textarea")info.controlType=UIA_EditControlTypeId;
        else if(node->tag==L"img")info.controlType=UIA_ImageControlTypeId;
        else if(node->tag==L"ul"||node->tag==L"ol")info.controlType=UIA_ListControlTypeId;
        else if(node->tag==L"li")info.controlType=UIA_ListItemControlTypeId;
        else if(node->tag==L"table")info.controlType=UIA_TableControlTypeId;
        else if(node->tag==L"th")info.controlType=UIA_HeaderItemControlTypeId;
        else if(node->tag==L"tr"||node->tag==L"td")info.controlType=UIA_DataItemControlTypeId;
        else if(node->type==NodeType::Text||(!node->tag.empty()&&node->tag[0]==L'h'&&node->tag.size()==2))info.controlType=UIA_TextControlTypeId;
        else info.controlType=UIA_GroupControlTypeId;
        info.toggle=(node->tag==L"input"&&(type==L"checkbox"||type==L"radio"))||role==L"checkbox"||role==L"radio"||role==L"switch";
        info.invoke=IsKeyboardActivatable(node)&&!info.toggle;info.valuePattern=IsTextInput(node)||node->tag==L"textarea"||role==L"textbox";
        info.selection=node->tag==L"select"||role==L"listbox";info.selectionItem=node->tag==L"option"||role==L"option";
        info.expandCollapse=node->attributes.count(L"aria-expanded")!=0||IsDatalistInput(node);
        if(IsTextInput(node)||node->tag==L"textarea"||role==L"textbox")info.value=node->Attribute(L"value");
        else if(node->tag==L"select")info.value=node->Attribute(L"value");
        if(info.selectionItem){
            const auto select=OwningSelect(node);if(select){const auto options=SelectOptions(select);if(!options.empty())info.selected=options[SelectedOptionIndex(select,options)]==node;}
            else info.selected=node->attributes.count(L"aria-selected")&&ToLower(node->Attribute(L"aria-selected"))==L"true";
        }
        auto boundsNode=node;if(node->tag==L"option")if(auto select=OwningSelect(node))boundsNode=select;
        const auto* box=layout.BoxFor(boundsNode);RECT client{};GetClientRect(hwnd,&client);POINT origin{0,0};ClientToScreen(hwnd,&origin);
        if(box&&box->visible){const float scale=DpiScale();info.bounds={origin.x+box->rect.x*scale,origin.y+box->rect.y*scale,box->rect.width*scale,box->rect.height*scale};
            info.offscreen=box->rect.width<=0||box->rect.height<=0||box->rect.x+box->rect.width<=0||box->rect.y+box->rect.height<=0||box->rect.x>=client.right/scale||box->rect.y>=client.bottom/scale;
        }else info.offscreen=true;
        return info;
    }
    void InitializeAccessibility(){
        accessibility=std::make_shared<AccessibilityHost>(hwnd);
        accessibility->SetCallbacks(
            [this](const auto& node){return AccessibilityInfo(node);},
            [this](const auto& node){return AccessibilityChildren(node);},
            [this](const auto& node){return AccessibilityParent(node);},
            [this](const auto& node,bool next){return AccessibilitySibling(node,next);},
            [this](double screenX,double screenY){POINT point{static_cast<LONG>(std::lround(screenX)),static_cast<LONG>(std::lround(screenY))};ScreenToClient(hwnd,&point);if(layoutDirty)Rebuild();auto node=layout.HitTest(PixelToDip(static_cast<float>(point.x)),PixelToDip(static_cast<float>(point.y)));while(node&&!IsAccessibilityNode(node))node=node->parent.lock();return node;},
            [this]{return focused;},
            [this](const auto& node)->HRESULT{if(!IsFocusable(node))return UIA_E_NOTSUPPORTED;SetFocus(hwnd);SetFocusedNode(node,true,true);return S_OK;},
            [this](const auto& node)->HRESULT{if(!node||!AccessibilityInfo(node).enabled)return UIA_E_ELEMENTNOTENABLED;if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);if(!box)return UIA_E_ELEMENTNOTAVAILABLE;Activate(node,box->rect.x+box->rect.width/2.0f,box->rect.y+box->rect.height/2.0f);return S_OK;},
            [this](const auto& node,const std::wstring& value)->HRESULT{if(!node||(!IsTextInput(node)&&node->tag!=L"textarea"&&ToLower(node->Attribute(L"role"))!=L"textbox"))return UIA_E_NOTSUPPORTED;if(node->disabled||node->attributes.count(L"readonly"))return UIA_E_ELEMENTNOTENABLED;const auto old=node->Attribute(L"value");if(old==value)return S_OK;node->SetAttribute(L"value",value);javascript.DispatchNodeEvent(node,L"input");javascript.DispatchNodeEvent(node,L"change");RefreshTextSelectionFromDom();layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return S_OK;},
            [this](const auto& node)->HRESULT{const auto select=OwningSelect(node);if(select){const auto options=SelectOptions(select);const auto it=std::find(options.begin(),options.end(),node);if(it==options.end()||node->disabled)return UIA_E_ELEMENTNOTENABLED;SelectOption(select,static_cast<size_t>(std::distance(options.begin(),it)));return S_OK;}if(node&&ToLower(node->Attribute(L"role"))==L"option"){const auto parent=node->parent.lock();if(!parent)return UIA_E_NOTSUPPORTED;for(const auto& sibling:parent->children)if(ToLower(sibling->Attribute(L"role"))==L"option")sibling->SetAttribute(L"aria-selected",sibling==node?L"true":L"false");javascript.DispatchNodeEvent(node,L"click");javascript.DispatchNodeEvent(node,L"change");layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return S_OK;}return UIA_E_NOTSUPPORTED;},
            [this](const auto& node)->HRESULT{if(!node)return UIA_E_ELEMENTNOTAVAILABLE;const auto role=ToLower(node->Attribute(L"role")),type=ToLower(node->Attribute(L"type"));const bool native=node->tag==L"input"&&(type==L"checkbox"||type==L"radio");if(!native&&role!=L"checkbox"&&role!=L"radio"&&role!=L"switch")return UIA_E_NOTSUPPORTED;const auto before=ToLower(node->Attribute(L"aria-checked"));if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);Activate(node,box?box->rect.x:0,box?box->rect.y:0);if(!native&&ToLower(node->Attribute(L"aria-checked"))==before)node->SetAttribute(L"aria-checked",before==L"true"?L"false":L"true");layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return S_OK;},
            [this](const auto& node,bool expand)->HRESULT{
                if(!node)return UIA_E_ELEMENTNOTAVAILABLE;
                if(IsDatalistInput(node)){
                    if(expand){SetFocus(hwnd);SetFocusedNode(node,true,true);OpenSelectPopup(node,true,false);}
                    else if(openSelectPopup==node)CloseSelectPopup();
                    return S_OK;
                }
                if(!node->attributes.count(L"aria-expanded"))return UIA_E_NOTSUPPORTED;const auto before=node->Attribute(L"aria-expanded");if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);Activate(node,box?box->rect.x:0,box?box->rect.y:0);if(ToLower(node->Attribute(L"aria-expanded"))==ToLower(before))node->SetAttribute(L"aria-expanded",expand?L"true":L"false");layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return S_OK;
            });
    }
    static bool IsWithin(const std::shared_ptr<Node>& node,const std::shared_ptr<Node>& ancestor){
        for(auto current=node;current;current=current->parent.lock())if(current==ancestor)return true;
        return false;
    }
    bool IsConnectedToDocument(const std::shared_ptr<Node>& node)const{
        if(!node)return false;
        auto current=node;while(auto parent=current->parent.lock())current=std::move(parent);
        return current==document.Root();
    }
    void RefreshLiveRegionIndex(const JavaScriptRuntime::Mutation& mutation,bool reset){
        std::vector<std::shared_ptr<Node>> indexed;
        auto add=[&](const std::shared_ptr<Node>& node){
            if(!node||node->attributes.count(L"aria-live")==0)return;
            if(std::find(indexed.begin(),indexed.end(),node)==indexed.end())indexed.push_back(node);
        };
        if(!reset){
            for(const auto& weak:liveRegions)if(auto node=weak.lock())
                if(IsConnectedToDocument(node)&&node->attributes.count(L"aria-live"))add(node);
        }
        if(reset||mutation.targets.empty()){
            indexed=document.QuerySelectorAll(L"[aria-live]");
        }else if(mutation.kind==JavaScriptRuntime::MutationKind::Tree||
                 mutation.liveRegionMembershipChanged){
            for(const auto& target:mutation.targets)
                for(const auto& node:document.QuerySelectorAll(L"[aria-live]",target))add(node);
        }
        liveRegions.clear();liveRegions.reserve(indexed.size());
        for(const auto& node:indexed)liveRegions.push_back(node);
    }
    void NotifyAccessibilityMutation(const JavaScriptRuntime::Mutation& mutation,bool reset=false){
        if(!accessibility)return;
        if(reset||mutation.kind==JavaScriptRuntime::MutationKind::Tree||
           mutation.liveRegionMembershipChanged)RefreshLiveRegionIndex(mutation,reset);
        std::unordered_set<const Node*> active;
        for(const auto& weak:liveRegions)if(auto node=weak.lock())active.insert(node.get());
        for(auto item=liveRegionText.begin();item!=liveRegionText.end();)
            if(active.count(item->first)==0)item=liveRegionText.erase(item);else ++item;
        if(liveRegions.empty()||(!reset&&mutation.kind!=JavaScriptRuntime::MutationKind::Tree&&
           !mutation.liveRegionMembershipChanged))return;
        for(const auto& weak:liveRegions){
            const auto node=weak.lock();if(!node)continue;
            bool affected=reset||mutation.liveRegionMembershipChanged||mutation.targets.empty();
            if(!affected)for(const auto& target:mutation.targets)
                if(IsWithin(target,node)||IsWithin(node,target)){affected=true;break;}
            if(!affected)continue;
            const auto text=Trim(node->InnerText());
            const auto previous=liveRegionText.find(node.get());
            if(previous!=liveRegionText.end()&&previous->second!=text)
                accessibility->RaiseLiveRegionChanged(node);
            liveRegionText[node.get()]=text;
        }
    }
    void HandleMutation(const JavaScriptRuntime::Mutation& mutation){
        LayoutRect dirtyBounds{};bool hasDirtyBounds=false,canTarget=!layoutDirty&&
            !mutation.targets.empty()&&(mutation.kind==JavaScriptRuntime::MutationKind::Paint||
                                        mutation.kind==JavaScriptRuntime::MutationKind::Style);
        if(canTarget&&mutation.kind==JavaScriptRuntime::MutationKind::Style&&
           (styles.HasPseudoRules(L"before")||styles.HasPseudoRules(L"after")||
            styles.MutationRequiresBroadInvalidation()))canTarget=false;
        if(canTarget)for(const auto& target:mutation.targets){
            if(mutation.kind==JavaScriptRuntime::MutationKind::Paint){
                const auto* box=layout.BoxFor(target);if(!box){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,box->rect);
            }else{
                LayoutRect visual{};if(!layout.VisualBounds(target,visual)){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,visual);
            }
        }
        if(mutation.kind==JavaScriptRuntime::MutationKind::Tree||
           mutation.kind==JavaScriptRuntime::MutationKind::Layout){
            layoutDirty=true;viewportOnlyDirty=false;
        }else if(mutation.kind==JavaScriptRuntime::MutationKind::Style&&!layoutDirty){
            bool layoutChanged=false;
            // A selector change can add or remove generated boxes. Restyling
            // updates existing boxes, so generated-content styles retain the
            // conservative tree-rebuild path.
            if(styles.HasPseudoRules(L"before")||styles.HasPseudoRules(L"after")){
                layoutChanged=true;
            }else if(styles.MutationRequiresBroadInvalidation()||mutation.targets.empty()){
                const auto* root=layout.Root();
                layoutChanged=!root||!root->node||layout.Restyle(root->node);
            }else{
                for(const auto& target:mutation.targets)
                    layoutChanged=layout.Restyle(target)||layoutChanged;
            }
            layoutDirty=layoutChanged;
            if(layoutDirty)viewportOnlyDirty=false;
            else SyncCssTransitionTimer();
        }
        if(!layoutDirty&&(mutation.kind==JavaScriptRuntime::MutationKind::Paint||
                          mutation.kind==JavaScriptRuntime::MutationKind::Style))
            for(const auto& target:mutation.targets)layout.SyncScroll(target);
        if(canTarget&&!layoutDirty&&mutation.kind==JavaScriptRuntime::MutationKind::Style)
            for(const auto& target:mutation.targets){
                LayoutRect visual{};if(!layout.VisualBounds(target,visual)){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,visual);
            }
        accessibilityTreeDirty=true;if(accessibility)accessibility->Invalidate();
        if(canTarget&&!layoutDirty&&hasDirtyBounds)InvalidateDipBounds(dirtyBounds);
        else InvalidateRect(hwnd,nullptr,FALSE);
        NotifyAccessibilityMutation(mutation);
    }
    void RestyleDocument(){
        if(layoutDirty)return;
        const auto* root=layout.Root();
        layoutDirty=!root||!root->node||layout.Restyle(root->node);
        if(layoutDirty)viewportOnlyDirty=false;else SyncCssTransitionTimer();
    }
    static std::wstring AccessibilityJsonEscape(const std::wstring& value){
        std::wstring out;for(const auto character:value){if(character==L'\\'||character==L'"')out+=L'\\';if(character==L'\n')out+=L"\\n";else if(character!=L'\r')out+=character;}return out;
    }
    std::wstring DumpAccessibilityJson(){
        if(layoutDirty)Rebuild();std::wstring output=L"[";bool first=true;
        std::function<void(const std::shared_ptr<Node>&)> append=[&](const std::shared_ptr<Node>& parent){
            for(const auto& node:AccessibilityChildren(parent)){
                const auto info=AccessibilityInfo(node);if(!first)output+=L",";first=false;
                std::wostringstream item;item<<L"{\"automationId\":\""<<AccessibilityJsonEscape(info.automationId)
                    <<L"\",\"name\":\""<<AccessibilityJsonEscape(info.name)<<L"\",\"role\":\""
                    <<AccessibilityJsonEscape(info.ariaRole)<<L"\",\"controlType\":"<<info.controlType
                    <<L",\"enabled\":"<<(info.enabled?L"true":L"false")<<L",\"focusable\":"
                    <<(info.focusable?L"true":L"false")<<L",\"x\":"<<std::lround(info.bounds.left)
                    <<L",\"y\":"<<std::lround(info.bounds.top)<<L",\"width\":"<<std::lround(info.bounds.width)
                    <<L",\"height\":"<<std::lround(info.bounds.height)<<L"}";output+=item.str();append(node);
            }
        };
        append({});return output+L"]";
    }
    HRESULT RenderBackBuffer(const LayoutRect& dirty){
        ID2D1RenderTarget* target=backBuffer.Get();
        if(!target)return E_INVALIDARG;target->BeginDraw();target->SetTransform(D2D1::IdentityMatrix());
        const auto dirtyRect=D2D1::RectF(dirty.x,dirty.y,dirty.x+dirty.width,dirty.y+dirty.height);
        target->PushAxisAlignedClip(dirtyRect,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ComPtr<ID2D1SolidColorBrush> background;
        if(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0xf3f5f8),&background)))
            target->FillRectangle(dirtyRect,background.Get());
        layout.Paint(target,writeFactory.Get(),&dirty);PaintTextEditing(target);PaintSelectPopup(target);
        target->PopAxisAlignedClip();
        return target->EndDraw();
    }
    void Paint(){
        PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EnsureTarget();
        if(renderTarget){
            // Build and paint the complete frame off-screen first. The HWND
            // target is resized only when the finished bitmap is ready, so a
            // live resize never exposes partially painted document content.
            if(layoutDirty)Rebuild();
            RECT client{};GetClientRect(hwnd,&client);const auto pixelSize=D2D1::SizeU(
                static_cast<UINT32>(std::max(1L,client.right-client.left)),
                static_cast<UINT32>(std::max(1L,client.bottom-client.top)));
            const float dpi=USER_DEFAULT_SCREEN_DPI*DpiScale();
            auto* previousBackBuffer=backBuffer.Get();
            if(EnsureBackBuffer(pixelSize,dpi)){
                const float scale=DpiScale();LayoutRect dirty;
                if(previousBackBuffer!=backBuffer.Get()||IsRectEmpty(&ps.rcPaint))
                    dirty={0,0,pixelSize.width/scale,pixelSize.height/scale};
                else{
                    const float left=std::floor(ps.rcPaint.left/scale),top=std::floor(ps.rcPaint.top/scale);
                    const float right=std::ceil(ps.rcPaint.right/scale),bottom=std::ceil(ps.rcPaint.bottom/scale);
                    dirty={left,top,std::max(0.0f,right-left),std::max(0.0f,bottom-top)};
                }
                const HRESULT rendered=RenderBackBuffer(dirty);
                if(SUCCEEDED(rendered)){
                    ComPtr<ID2D1Bitmap> bitmap;
                    if(SUCCEEDED(backBuffer->GetBitmap(&bitmap))){
                        const auto currentSize=renderTarget->GetPixelSize();
                        const HRESULT resized=currentSize.width==pixelSize.width&&currentSize.height==pixelSize.height?
                            S_OK:renderTarget->Resize(pixelSize);
                        if(SUCCEEDED(resized)){
                            renderTarget->SetDpi(dpi,dpi);
                            const auto dirtyBounds=D2D1::RectF(dirty.x,dirty.y,
                                dirty.x+dirty.width,dirty.y+dirty.height);
                            renderTarget->BeginDraw();renderTarget->SetTransform(D2D1::IdentityMatrix());
                            renderTarget->PushAxisAlignedClip(dirtyBounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                            renderTarget->DrawBitmap(bitmap.Get(),dirtyBounds,1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,dirtyBounds);
                            renderTarget->PopAxisAlignedClip();
                            const HRESULT result=renderTarget->EndDraw();
                            if(result==D2DERR_RECREATE_TARGET)ResetRenderTargets();
                        }else ResetRenderTargets();
                    }
                }else if(rendered==D2DERR_RECREATE_TARGET)ResetRenderTargets();
            }
        }
        EndPaint(hwnd,&ps);
    }
    std::shared_ptr<Node> FocusTarget(const std::shared_ptr<Node>& node)const{
        if(IsTextControl(node))return node;
        if(auto root=EditableRoot(node))return root;
        return node;
    }
    bool CanEditText()const{
        return IsEditableTextControl(focused)||(focused&&IsContentEditable(focused)&&editingNode&&
            editingNode->type==NodeType::Text);
    }
    std::wstring EditingValue()const{
        if(!editingNode)return {};
        return editingNode->type==NodeType::Text?editingNode->text:editingNode->Attribute(L"value");
    }
    void SetEditingValue(const std::wstring& value){
        if(!editingNode)return;
        if(editingNode->type==NodeType::Text)editingNode->text=value;
        else editingNode->SetAttribute(L"value",value);
    }
    void StoreControlSelection(){
        if(IsTextControl(focused)){
            focused->selectionStart=std::min(selectionAnchor,caretPosition);
            focused->selectionEnd=std::max(selectionAnchor,caretPosition);
        }
    }
    std::shared_ptr<Node> FirstEditableText(const std::shared_ptr<Node>& parent)const{
        if(!parent)return {};
        if(parent->type==NodeType::Text)return parent;
        if(parent!=focused&&parent->attributes.count(L"contenteditable")&&!IsContentEditable(parent))return {};
        for(const auto& child:parent->children)if(auto text=FirstEditableText(child))return text;
        return {};
    }
    std::shared_ptr<Node> EnsureEditableTextNode(const std::shared_ptr<Node>& target){
        if(IsTextControl(focused))return focused;
        auto candidate=target;
        while(candidate&&candidate->type!=NodeType::Text&&candidate!=focused)candidate=candidate->parent.lock();
        if(candidate&&candidate->type==NodeType::Text&&EditableRoot(candidate)==focused)return candidate;
        if(auto text=FirstEditableText(target&&EditableRoot(target)==focused?target:focused))return text;
        if(!focused||!IsContentEditable(focused))return {};
        auto text=std::make_shared<Node>();text->type=NodeType::Text;text->text=L"";text->parent=focused;
        focused->children.push_back(text);document.Reindex();layoutDirty=true;return text;
    }
    const LayoutBox* EditingLayoutBox()const{
        return editingNode?layout.BoxFor(editingNode):nullptr;
    }
    const LayoutBox* EditingStyleBox()const{
        if(!editingNode)return nullptr;
        for(auto current=editingNode;current;current=current->parent.lock())
            if(const auto* box=layout.BoxFor(current))return box;
        return nullptr;
    }
    void BeginEditingAt(const std::shared_ptr<Node>& target,float x,float y){
        if(layoutDirty)Rebuild();std::shared_ptr<Node> hitNode;size_t hitOffset=0;
        auto scope=IsTextControl(focused)?focused:
            (target&&EditableRoot(target)==focused?target:focused);
        bool hit=layout.HitTestText(scope,x,y,hitNode,hitOffset);
        if(!hit&&scope!=focused)hit=layout.HitTestText(focused,x,y,hitNode,hitOffset);
        if(hit&&(hitNode==focused||EditableRoot(hitNode)==focused)){
            editingNode=std::move(hitNode);selectionAnchor=caretPosition=hitOffset;
        }else{
            editingNode=EnsureEditableTextNode(target);if(!editingNode)return;
            selectionAnchor=caretPosition=EditingValue().size();
        }
        StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();
    }
    bool UpdateTextSelectionAt(float x,float y){
        if(!textSelectionDragging||!editingNode||!focused)return false;
        if(layoutDirty)Rebuild();
        std::shared_ptr<Node> hitNode;size_t hitOffset=0;
        const auto scope=IsTextControl(focused)?focused:editingNode;
        if(!layout.HitTestText(scope,x,y,hitNode,hitOffset)||hitNode!=editingNode)return false;
        const auto next=std::min(hitOffset,EditingValue().size());
        if(caretPosition==next)return true;
        caretPosition=next;StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();return true;
    }
    bool CanBlinkCaret()const{
        return hwnd&&editingNode&&focused&&GetFocus()==hwnd&&
            (selectionAnchor==caretPosition||compositionActive);
    }
    void StopCaretBlink(){
        if(caretBlinkTimerActive){KillTimer(hwnd,kCaretBlinkTimer);caretBlinkTimerActive=false;}
        const bool changed=!caretVisible;caretVisible=true;
        if(changed&&hwnd)InvalidateRect(hwnd,nullptr,FALSE);
    }
    void ResetCaretBlink(){
        if(caretBlinkTimerActive){KillTimer(hwnd,kCaretBlinkTimer);caretBlinkTimerActive=false;}
        caretVisible=true;
        if(CanBlinkCaret()){
            UINT interval=GetCaretBlinkTime();
            if(interval==0)interval=530;
            if(interval!=INFINITE)
                caretBlinkTimerActive=SetTimer(hwnd,kCaretBlinkTimer,
                    std::max(interval,static_cast<UINT>(USER_TIMER_MINIMUM)),nullptr)!=0;
        }
        if(hwnd)InvalidateRect(hwnd,nullptr,FALSE);
    }
    bool CaretDipRect(D2D1_RECT_F& result){
        if(!editingNode)return false;LayoutRect caret{};
        if(!layout.TextCaretRect(editingNode,caretPosition,caret))return false;
        result=D2D1::RectF(caret.x,caret.y,caret.x+caret.width,caret.y+caret.height);return true;
    }
    RECT CaretClientRect(){
        if(layoutDirty)Rebuild();D2D1_RECT_F caret{};if(!CaretDipRect(caret))return RECT{};
        return RECT{DipToPixel(caret.left),DipToPixel(caret.top),
                    DipToPixel(caret.right),DipToPixel(caret.bottom)};
    }
    void InvalidateCaret(){
        if(!hwnd)return;
        RECT caret=CaretClientRect();
        if(IsRectEmpty(&caret)){InvalidateRect(hwnd,nullptr,FALSE);return;}
        InflateRect(&caret,2,2);InvalidateRect(hwnd,&caret,FALSE);
    }
    void PaintTextEditing(ID2D1RenderTarget* target){
        if(!target||!editingNode||GetFocus()!=hwnd)return;const auto* box=EditingLayoutBox();
        const auto* styleBox=EditingStyleBox();if(!styleBox||!styleBox->visible)return;
        const size_t valueLength=EditingValue().size();
        const size_t begin=std::min({selectionAnchor,caretPosition,valueLength});
        const size_t end=std::min(valueLength,std::max(selectionAnchor,caretPosition));
        ComPtr<ID2D1SolidColorBrush> brush;
        if(box&&box->textLayout&&end>begin){UINT32 count=0;box->textLayout->HitTestTextRange(static_cast<UINT32>(begin),
                static_cast<UINT32>(end-begin),box->content.x,box->content.y,nullptr,0,&count);
            if(count){std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);if(SUCCEEDED(
                box->textLayout->HitTestTextRange(static_cast<UINT32>(begin),static_cast<UINT32>(end-begin),
                    box->content.x,box->content.y,metrics.data(),count,&count))){
                target->CreateSolidColorBrush(D2D1::ColorF(0x3b82f6,0.36f),&brush);
                for(UINT32 index=0;index<count;++index){const auto& hit=metrics[index];
                    target->FillRectangle(D2D1::RectF(hit.left,hit.top,hit.left+hit.width,
                                                      hit.top+hit.height),brush.Get());
                }
            }}
        }
        if(box&&box->textLayout&&compositionActive&&!compositionText.empty()){
            UINT32 count=0;box->textLayout->HitTestTextRange(static_cast<UINT32>(compositionReplaceStart),
                static_cast<UINT32>(compositionText.size()),box->content.x,box->content.y,nullptr,0,&count);
            if(count){std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);if(SUCCEEDED(
                box->textLayout->HitTestTextRange(static_cast<UINT32>(compositionReplaceStart),
                    static_cast<UINT32>(compositionText.size()),box->content.x,box->content.y,
                    metrics.data(),count,&count))){
                target->CreateSolidColorBrush(D2D1::ColorF(0xff2563eb),&brush);
                for(UINT32 index=0;index<count;++index){const auto& hit=metrics[index];
                    target->DrawLine(D2D1::Point2F(hit.left,hit.top+hit.height-1.0f/DpiScale()),
                        D2D1::Point2F(hit.left+hit.width,hit.top+hit.height-1.0f/DpiScale()),
                        brush.Get(),1.0f/DpiScale());
                }
            }}
        }
        if((selectionAnchor==caretPosition||compositionActive)&&caretVisible){D2D1_RECT_F caret{};if(CaretDipRect(caret)){
            const auto color=StyleSheet::Color(styleBox->style.Get(L"caret-color",styleBox->style.Get(L"color",L"#000")),0xff000000);
            target->CreateSolidColorBrush(D2D1::ColorF(color&0x00ffffff),&brush);target->FillRectangle(caret,brush.Get());
        }}
    }
    void RefreshTextSelectionFromDom(){
        if(!editingNode||compositionActive)return;const auto length=EditingValue().size();
        selectionAnchor=std::min(selectionAnchor,length);caretPosition=std::min(caretPosition,length);
        StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);
    }
    void BeginTextComposition(){
        if(!CanEditText()||compositionActive)return;compositionActive=true;compositionBase=EditingValue();
        compositionReplaceStart=std::min(selectionAnchor,caretPosition);
        compositionReplaceEnd=std::min(compositionBase.size(),std::max(selectionAnchor,caretPosition));
        compositionText.clear();compositionAttributes.clear();compositionCursor=0;
        JavaScriptRuntime::EventInit event{};event.isComposing=true;
        javascript.DispatchNodeEvent(focused,L"compositionstart",event);
    }
    void UpdateTextComposition(const std::wstring& text,const std::vector<unsigned char>& attributes,
                               size_t cursor){
        if(!compositionActive)BeginTextComposition();if(!compositionActive)return;
        auto display=compositionBase;display.replace(compositionReplaceStart,
            compositionReplaceEnd-compositionReplaceStart,text);SetEditingValue(display);
        compositionText=text;compositionAttributes=attributes;compositionCursor=std::min(cursor,text.size());
        selectionAnchor=caretPosition=compositionReplaceStart+compositionCursor;
        StoreControlSelection();
        JavaScriptRuntime::EventInit event{};event.data=text;event.isComposing=true;
        javascript.DispatchNodeEvent(focused,L"compositionupdate",event);
        layoutDirty=true;ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);
    }
    void CommitTextComposition(const std::wstring& text){
        if(!compositionActive){ReplaceSelection(text,L"insertCompositionText",true);return;}
        SetEditingValue(compositionBase);selectionAnchor=compositionReplaceStart;caretPosition=compositionReplaceEnd;
        StoreControlSelection();
        compositionActive=false;compositionText.clear();compositionAttributes.clear();
        JavaScriptRuntime::EventInit ended{};ended.data=text;
        javascript.DispatchNodeEvent(focused,L"compositionend",ended);
        ReplaceSelection(text,L"insertCompositionText",false);
    }
    void CancelTextComposition(){
        if(!compositionActive)return;SetEditingValue(compositionBase);
        selectionAnchor=compositionReplaceStart;caretPosition=compositionReplaceEnd;compositionActive=false;
        StoreControlSelection();
        compositionText.clear();compositionAttributes.clear();
        javascript.DispatchNodeEvent(focused,L"compositionend");layoutDirty=true;
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void CommitTextEdit(){if(IsTextControl(focused)&&textEditDirty){javascript.DispatchNodeEvent(focused,L"change");textEditDirty=false;}}
    bool SetHoveredNode(const std::shared_ptr<Node>& next,LayoutRect* dirtyBounds=nullptr,
                        bool* dirtyBoundsValid=nullptr){
        if(dirtyBoundsValid)*dirtyBoundsValid=false;
        if(next==hovered)return false;
        std::vector<std::shared_ptr<Node>> nextPath;
        for(auto current=next;current;current=current->parent.lock())nextPath.push_back(current);
        size_t previousUnique=hoverPath.size(),nextUnique=nextPath.size();
        while(previousUnique&&nextUnique&&hoverPath[previousUnique-1]==nextPath[nextUnique-1]){
            --previousUnique;--nextUnique;
        }
        std::vector<std::shared_ptr<Node>> affected;
        auto collectAffected=[&](const std::vector<std::shared_ptr<Node>>& path,size_t count){
            std::shared_ptr<Node> outermost;
            for(size_t index=0;index<count;++index)
                if(styles.HoverStateAffects(path[index]))outermost=path[index];
            if(outermost&&std::find(affected.begin(),affected.end(),outermost)==affected.end())
                affected.push_back(std::move(outermost));
        };
        collectAffected(hoverPath,previousUnique);
        collectAffected(nextPath,nextUnique);
        for(size_t index=0;index<previousUnique;++index)hoverPath[index]->hovered=false;
        for(size_t index=0;index<nextUnique;++index)nextPath[index]->hovered=true;

        hovered=next;hoverPath=std::move(nextPath);
        if(affected.empty())return false;
        if(styles.HoverRequiresBroadInvalidation()){
            const auto* root=layout.Root();
            return root&&root->node?layout.Restyle(root->node):false;
        }
        LayoutRect dirty{};bool hasDirty=false,safe=true;
        if(dirtyBounds)for(const auto& node:affected){
            LayoutRect visual{};if(!layout.VisualBounds(node,visual)){safe=false;break;}
            IncludeBounds(dirty,hasDirty,visual);
        }
        bool layoutChanged=false;
        for(const auto& node:affected)
            if(layout.BoxFor(node))layoutChanged=layout.Restyle(node)||layoutChanged;
        if(dirtyBounds&&safe&&!layoutChanged)for(const auto& node:affected){
            LayoutRect visual{};if(!layout.VisualBounds(node,visual)){safe=false;break;}
            IncludeBounds(dirty,hasDirty,visual);
        }
        if(dirtyBounds&&dirtyBoundsValid&&safe&&hasDirty&&!layoutChanged){
            *dirtyBounds=dirty;*dirtyBoundsValid=true;
        }
        return layoutChanged;
    }
    void SetFocusedNode(const std::shared_ptr<Node>& node,bool focusVisible=false,
                        bool openTextEditor=false){
        const auto candidate=FocusTarget(node);const auto next=IsFocusable(candidate)?candidate:std::shared_ptr<Node>{};
        if(openSelectPopup&&next!=openSelectPopup)CloseSelectPopup();
        if(next==focused){
            const bool desired=focused&&(focusVisible||IsTextControl(focused)||IsContentEditable(focused));
            if(focused&&desired!=focused->focusVisible){focused->focusVisible=desired;RestyleDocument();if(accessibility)accessibility->Invalidate();InvalidateRect(hwnd,nullptr,FALSE);}
            if(focused&&openTextEditor&&!editingNode&&(IsTextControl(focused)||IsContentEditable(focused)))
                editingNode=EnsureEditableTextNode(focused);
            if(editingNode)ResetCaretBlink();
            return;
        }
        textInput.Cancel(hwnd);StoreControlSelection();editingNode.reset();StopCaretBlink();
        if(focused){for(auto current=focused;current;current=current->parent.lock())current->focusWithin=false;CommitTextEdit();focused->focused=false;focused->focusVisible=false;
            javascript.DispatchNodeEvent(focused,L"blur");javascript.DispatchNodeEvent(focused,L"focusout");}
        focused=next;textEditDirty=false;selectionAnchor=caretPosition=0;
        undoHistory.clear();redoHistory.clear();
        if(focused){focused->focused=true;focused->focusVisible=focusVisible||IsTextControl(focused)||IsContentEditable(focused);for(auto current=focused;current;current=current->parent.lock())current->focusWithin=true;
            if(IsTextControl(focused)){editingNode=focused;const auto length=EditingValue().size();selectionAnchor=std::min(focused->selectionStart,length);caretPosition=std::min(focused->selectionEnd,length);}
            else if(IsContentEditable(focused)){editingNode=EnsureEditableTextNode(focused);selectionAnchor=caretPosition=EditingValue().size();}
            javascript.DispatchNodeEvent(focused,L"focus");javascript.DispatchNodeEvent(focused,L"focusin");}
        RestyleDocument();if(accessibility)accessibility->Invalidate();InvalidateRect(hwnd,nullptr,FALSE);
        if(focused&&accessibility)accessibility->RaiseFocusChanged(focused);
        if(focused&&openTextEditor&&(IsTextControl(focused)||IsContentEditable(focused)))
            textInput.UpdateCandidateWindow(hwnd);
        ResetCaretBlink();
    }
    std::vector<std::shared_ptr<Node>> SequentialFocusOrder(){
        if(layoutDirty)Rebuild();
        struct Candidate { std::shared_ptr<Node> node; int tabIndex=0; size_t order=0; };
        std::vector<Candidate> candidates;size_t order=0;
        std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& parent){
            if(!parent)return;
            if(parent->type==NodeType::Element){
                const int tabIndex=SequentialTabIndex(parent);const auto* box=layout.BoxFor(parent);
                if(tabIndex>=0&&box&&box->visible&&box->rect.width>0&&box->rect.height>0)
                    candidates.push_back({parent,tabIndex,order});
                ++order;
            }
            for(const auto& child:parent->children)visit(child);
        };
        visit(document.Root());
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& left,const Candidate& right){
            const bool leftPositive=left.tabIndex>0,rightPositive=right.tabIndex>0;
            if(leftPositive!=rightPositive)return leftPositive;
            if(leftPositive&&left.tabIndex!=right.tabIndex)return left.tabIndex<right.tabIndex;
            return left.order<right.order;
        });
        std::vector<std::shared_ptr<Node>> result;result.reserve(candidates.size());
        for(auto& candidate:candidates)result.push_back(std::move(candidate.node));
        return result;
    }
    bool MoveSequentialFocus(bool reverse){
        auto order=SequentialFocusOrder();if(order.empty())return false;
        auto current=std::find(order.begin(),order.end(),focused);size_t index=0;
        if(current==order.end())index=reverse?order.size()-1:0;
        else{
            const size_t old=static_cast<size_t>(std::distance(order.begin(),current));
            index=reverse?(old==0?order.size()-1:old-1):(old+1)%order.size();
        }
        SetFocus(hwnd);SetFocusedNode(order[index],true,true);return true;
    }
    bool MoveMenuFocus(WPARAM key){
        if(!focused)return false;
        auto associatedMenu=[this](const std::shared_ptr<Node>& trigger){
            const auto controls=trigger->Attribute(L"aria-controls");if(!controls.empty())if(auto menu=document.GetElementById(controls))return menu;
            const auto parent=trigger->parent.lock();if(parent)for(const auto& child:parent->children)
                if(ToLower(child->Attribute(L"role"))==L"menu")return child;
            return std::shared_ptr<Node>{};
        };
        auto menuTrigger=[&](const std::shared_ptr<Node>& menu){
            const auto labelled=menu?menu->Attribute(L"aria-labelledby"):L"";if(!labelled.empty())if(auto trigger=document.GetElementById(labelled))return trigger;
            const auto parent=menu?menu->parent.lock():std::shared_ptr<Node>{};if(parent)for(const auto& child:parent->children)
                if(child!=menu&&IsKeyboardActivatable(child)&&ToLower(child->Attribute(L"role"))!=L"menuitem")return child;
            return std::shared_ptr<Node>{};
        };
        auto itemsFor=[this](const std::shared_ptr<Node>& menu){
            std::vector<std::shared_ptr<Node>> items;
            std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& parent){
            for(const auto& child:parent->children){
                if(ToLower(child->Attribute(L"role"))==L"menuitem"&&IsFocusable(child)){
                    const auto* box=layout.BoxFor(child);if(box&&box->visible)items.push_back(child);
                }
                visit(child);
            }
            };
            if(menu)visit(menu);return items;
        };
        auto openAndFocus=[&](const std::shared_ptr<Node>& trigger,const std::shared_ptr<Node>& menu,bool last){
            if(!trigger||!menu)return false;if(layoutDirty)Rebuild();auto items=itemsFor(menu);
            if(items.empty()){
                const auto* box=layout.BoxFor(trigger);Activate(trigger,box?box->rect.x:0,box?box->rect.y:0,true);
                if(layoutDirty)Rebuild();items=itemsFor(menu);
            }
            if(items.empty())return false;SetFocusedNode(last?items.back():items.front(),true,false);return true;
        };
        const auto role=ToLower(focused->Attribute(L"role"));
        if(role!=L"menuitem"){
            const auto menu=associatedMenu(focused);
            if(menu&&(key==VK_DOWN||key==VK_UP||key==VK_HOME||key==VK_END))
                return openAndFocus(focused,menu,key==VK_UP||key==VK_END);
            return false;
        }
        auto menu=focused->parent.lock();while(menu&&ToLower(menu->Attribute(L"role"))!=L"menu"&&ToLower(menu->Attribute(L"role"))!=L"menubar")menu=menu->parent.lock();
        if(key==VK_ESCAPE){if(auto trigger=menuTrigger(menu)){SetFocusedNode(trigger,true,false);return true;}return false;}
        if(!menu)return false;if(layoutDirty)Rebuild();auto items=itemsFor(menu);if(items.empty())return false;
        if((key==VK_LEFT||key==VK_RIGHT)&&ToLower(menu->Attribute(L"role"))!=L"menubar"){
            const auto root=menu->parent.lock(),container=root?root->parent.lock():std::shared_ptr<Node>{};
            if(container){std::vector<std::pair<std::shared_ptr<Node>,std::shared_ptr<Node>>> roots;
                for(const auto& child:container->children)for(const auto& descendant:child->children)
                    if(ToLower(descendant->Attribute(L"role"))==L"menu"){if(auto trigger=menuTrigger(descendant))roots.push_back({child,trigger});break;}
                const auto found=std::find_if(roots.begin(),roots.end(),[&](const auto& item){return item.first==root;});
                if(found!=roots.end()&&roots.size()>1){size_t index=static_cast<size_t>(std::distance(roots.begin(),found));index=key==VK_LEFT?(index==0?roots.size()-1:index-1):(index+1)%roots.size();const auto trigger=roots[index].second;return openAndFocus(trigger,associatedMenu(trigger),false);}}
            return false;
        }
        auto current=std::find(items.begin(),items.end(),focused);
        size_t index=current==items.end()?0:static_cast<size_t>(std::distance(items.begin(),current));
        if(key==VK_HOME)index=0;else if(key==VK_END)index=items.size()-1;
        else if(key==VK_UP||key==VK_LEFT)index=index==0?items.size()-1:index-1;
        else if(key==VK_DOWN||key==VK_RIGHT)index=(index+1)%items.size();else return false;
        SetFocusedNode(items[index],true,false);return true;
    }
    std::shared_ptr<Node> DatalistForInput(const std::shared_ptr<Node>& input)const{
        if(!IsTextInput(input))return {};
        const auto id=input->Attribute(L"list");if(id.empty())return {};
        const auto datalist=document.GetElementById(id);
        return datalist&&datalist->tag==L"datalist"?datalist:std::shared_ptr<Node>{};
    }
    bool IsDatalistInput(const std::shared_ptr<Node>& input)const{
        return static_cast<bool>(DatalistForInput(input));
    }
    std::vector<std::shared_ptr<Node>> DatalistOptions(const std::shared_ptr<Node>& input,
                                                        bool showAll)const{
        std::vector<std::shared_ptr<Node>> result;const auto datalist=DatalistForInput(input);
        if(!datalist)return result;
        const auto query=ToLower(input->Attribute(L"value"));
        std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
            for(const auto& child:parent->children){
                if(child->tag!=L"option"){collect(child);continue;}
                const auto value=OptionValue(child),label=child->Attribute(L"label");
                if(showAll||query.empty()||ToLower(value).find(query)==0||
                   (!label.empty()&&ToLower(label).find(query)==0))result.push_back(child);
            }
        };
        collect(datalist);return result;
    }
    std::vector<std::shared_ptr<Node>> PopupOptions(const std::shared_ptr<Node>& control)const{
        return control&&control->tag==L"select"?SelectOptions(control):
            DatalistOptions(control,selectPopupShowAll);
    }
    int PopupSelectedIndex(const std::shared_ptr<Node>& control,
                           const std::vector<std::shared_ptr<Node>>& options)const{
        if(options.empty())return -1;
        if(control&&control->tag==L"select")return static_cast<int>(SelectedOptionIndex(control,options));
        const auto value=control?control->Attribute(L"value"):L"";
        for(size_t index=0;index<options.size();++index)
            if(OptionValue(options[index])==value)return static_cast<int>(index);
        return -1;
    }
    bool SelectOption(const std::shared_ptr<Node>& select,size_t index,bool dispatchEvents=true){
        const auto options=SelectOptions(select);if(!select||index>=options.size()||options[index]->disabled)return false;
        const size_t previous=SelectedOptionIndex(select,options);if(previous==index)return false;
        for(auto& option:options)option->RemoveAttribute(L"selected");
        options[index]->SetAttribute(L"selected",L"");
        select->SetAttribute(L"value",OptionValue(options[index]));
        if(dispatchEvents){javascript.DispatchNodeEvent(select,L"input");javascript.DispatchNodeEvent(select,L"change");}
        layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return true;
    }
    bool ChoosePopupOption(const std::shared_ptr<Node>& control,size_t index){
        const auto options=PopupOptions(control);if(!control||index>=options.size()||options[index]->disabled)return false;
        if(control->tag==L"select"){
            const auto all=SelectOptions(control);const auto found=std::find(all.begin(),all.end(),options[index]);
            return found!=all.end()&&SelectOption(control,static_cast<size_t>(std::distance(all.begin(),found)));
        }
        if(!IsDatalistInput(control))return false;
        const auto value=OptionValue(options[index]);const bool changed=value!=control->Attribute(L"value");
        control->SetAttribute(L"value",value);selectionAnchor=caretPosition=value.size();
        if(focused==control)editingNode=control;StoreControlSelection();RefreshTextSelectionFromDom();textEditDirty=false;
        if(changed){javascript.DispatchNodeEvent(control,L"input");javascript.DispatchNodeEvent(control,L"change");}
        layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return changed;
    }
    bool MoveSelectOption(const std::shared_ptr<Node>& select,int direction,bool toEdge=false){
        const auto options=SelectOptions(select);if(options.empty())return false;
        size_t index=SelectedOptionIndex(select,options);
        if(toEdge)index=direction<0?0:options.size()-1;
        else{
            const auto next=static_cast<long long>(index)+direction;
            index=static_cast<size_t>(std::max<long long>(0,std::min<long long>(
                static_cast<long long>(options.size()-1),next)));
        }
        while(index<options.size()&&options[index]->disabled){
            if(direction<0){if(index==0)return false;--index;}
            else{if(index+1>=options.size())return false;++index;}
        }
        return SelectOption(select,index);
    }
    struct SelectPopupGeometry {
        LayoutRect bounds;
        float rowHeight=0;
        float borderWidth=0;
        float textInset=0;
        float contentHeight=0;
        float viewportHeight=0;
        float scrollOffset=0;
        float scrollbarWidth=0;
    };
    bool GetSelectPopupGeometry(const std::shared_ptr<Node>& control,SelectPopupGeometry& geometry)const{
        if(!control)return false;const auto* box=layout.BoxFor(control);if(!box||!box->visible)return false;
        const auto options=PopupOptions(control);if(options.empty())return false;
        RECT client{};GetClientRect(hwnd,&client);const float scale=DpiScale();
        const float viewportWidth=std::max(1.0f,static_cast<float>(client.right-client.left)/scale);
        const float viewportHeight=std::max(1.0f,static_cast<float>(client.bottom-client.top)/scale);
        auto metric=[&](const wchar_t* property,float reference,float fallback){
            const auto raw=box->style.Get(property);return raw.empty()?fallback:
                StyleSheet::Length(raw,reference,viewportWidth,fallback);
        };
        const bool editable=IsDatalistInput(control);
        geometry.rowHeight=std::max(1.0f,metric(L"--select-menu-row-height",box->rect.height,
            editable?std::max(36.0f,box->rect.height*1.25f):box->rect.height*0.75f));
        geometry.borderWidth=std::max(0.0f,metric(L"--select-menu-border-width",box->rect.width,1.0f));
        geometry.textInset=std::max(0.0f,metric(L"--select-menu-text-inset",box->rect.width,box->rect.height*0.3125f));
        const float gap=std::max(0.0f,metric(L"--select-menu-gap",box->rect.height,box->rect.height*0.125f));
        const float width=std::max(1.0f,metric(L"--select-menu-width",box->rect.width,box->rect.width));
        float maxRows=editable?5.0f:8.0f;const auto rawMaxRows=box->style.Get(L"--select-menu-max-rows");
        if(!rawMaxRows.empty())try{maxRows=std::max(1.0f,std::stof(rawMaxRows));}catch(...){}
        geometry.contentHeight=geometry.rowHeight*static_cast<float>(options.size());
        geometry.viewportHeight=std::min(geometry.contentHeight,geometry.rowHeight*maxRows);
        float height=geometry.viewportHeight+geometry.borderWidth*2.0f;
        float left=std::max(0.0f,std::min(box->rect.x,viewportWidth-width));
        float top=box->rect.y+box->rect.height+gap;
        const float roomBelow=std::max(0.0f,viewportHeight-top),roomAbove=std::max(0.0f,box->rect.y-gap);
        if(height>roomBelow&&roomAbove>roomBelow){height=std::min(height,roomAbove);top=box->rect.y-gap-height;}
        else height=std::min(height,roomBelow);
        geometry.viewportHeight=std::max(1.0f,height-geometry.borderWidth*2.0f);
        geometry.scrollOffset=std::max(0.0f,std::min(selectPopupScrollOffset,
            std::max(0.0f,geometry.contentHeight-geometry.viewportHeight)));
        geometry.scrollbarWidth=geometry.contentHeight>geometry.viewportHeight+0.5f?10.0f:0.0f;
        geometry.bounds={left,top,std::min(width,viewportWidth),height};return height>geometry.borderWidth*2.0f;
    }
    int SelectPopupIndexAt(float x,float y)const{
        SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(openSelectPopup,geometry)||!geometry.bounds.Contains(x,y))return -1;
        if(geometry.scrollbarWidth>0&&x>=geometry.bounds.x+geometry.bounds.width-geometry.borderWidth-geometry.scrollbarWidth)return -1;
        const float local=y-geometry.bounds.y-geometry.borderWidth;
        if(local<0||local>=geometry.viewportHeight)return -1;
        const int index=static_cast<int>(std::floor((local+geometry.scrollOffset)/geometry.rowHeight));
        return index>=0&&static_cast<size_t>(index)<PopupOptions(openSelectPopup).size()?index:-1;
    }
    void CloseSelectPopup(){
        if(!openSelectPopup)return;openSelectPopup.reset();selectPopupHotIndex=-1;selectPopupScrollOffset=0;
        selectPopupShowAll=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;
        if(GetCapture()==hwnd)ReleaseCapture();InvalidateRect(hwnd,nullptr,FALSE);
    }
    void EnsureSelectPopupHotVisible(){
        if(selectPopupHotIndex<0)return;SelectPopupGeometry geometry;
        if(!GetSelectPopupGeometry(openSelectPopup,geometry))return;
        const float top=geometry.rowHeight*selectPopupHotIndex,bottom=top+geometry.rowHeight;
        if(top<geometry.scrollOffset)selectPopupScrollOffset=top;
        else if(bottom>geometry.scrollOffset+geometry.viewportHeight)
            selectPopupScrollOffset=bottom-geometry.viewportHeight;
    }
    bool MoveSelectPopupHot(int direction,bool toEdge=false){
        const auto options=PopupOptions(openSelectPopup);if(options.empty())return false;
        int index=selectPopupHotIndex;
        if(toEdge)index=direction<0?0:static_cast<int>(options.size()-1);
        else if(index<0)index=direction<0?static_cast<int>(options.size()-1):0;
        else index=std::max(0,std::min(static_cast<int>(options.size()-1),index+direction));
        while(index>=0&&static_cast<size_t>(index)<options.size()&&options[index]->disabled){
            index+=direction<0?-1:1;
        }
        if(index<0||static_cast<size_t>(index)>=options.size())return false;
        if(index!=selectPopupHotIndex){selectPopupHotIndex=index;EnsureSelectPopupHotVisible();InvalidateRect(hwnd,nullptr,FALSE);}return true;
    }
    bool ScrollSelectPopup(float amount){
        SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(openSelectPopup,geometry))return false;
        const float maximum=std::max(0.0f,geometry.contentHeight-geometry.viewportHeight);
        const float previous=selectPopupScrollOffset;
        selectPopupScrollOffset=std::max(0.0f,std::min(maximum,previous+amount));
        if(std::abs(previous-selectPopupScrollOffset)<0.01f)return false;
        InvalidateRect(hwnd,nullptr,FALSE);return true;
    }
    void PaintSelectPopup(ID2D1RenderTarget* target){
        SelectPopupGeometry geometry;if(!target||!writeFactory||!GetSelectPopupGeometry(openSelectPopup,geometry)){openSelectPopup.reset();selectPopupHotIndex=-1;return;}
        const auto options=PopupOptions(openSelectPopup);if(options.empty())return;
        const auto* box=layout.BoxFor(openSelectPopup);if(!box)return;
        auto d2d=[](unsigned int value){return D2D1::ColorF(((value>>16)&255)/255.0f,((value>>8)&255)/255.0f,(value&255)/255.0f,((value>>24)&255)/255.0f);};
        // A native select popup belongs to the control's rendered color scheme.
        // Use the computed control surface unless the author explicitly supplies
        // one of the popup customization properties.
        auto popupSurface=EffectiveBackgroundColor(box);
        if(IsDatalistInput(openSelectPopup)){
            auto scheme=ToLower(Trim(box->style.Get(L"color-scheme",L"light")));
            const auto separator=scheme.find_first_of(L" \t");if(separator!=std::wstring::npos)scheme.resize(separator);
            popupSurface=scheme==L"dark"?0xff202020u:0xffffffffu;
        }
        const auto palette=ResolveSelectPopupPalette(box->style,popupSurface);
        ComPtr<ID2D1SolidColorBrush> brush;
        const auto outer=D2D1::RectF(geometry.bounds.x,geometry.bounds.y,
            geometry.bounds.x+geometry.bounds.width,geometry.bounds.y+geometry.bounds.height);
        const auto rawShadowSize=box->style.Get(L"--select-menu-shadow-size");
        const float shadowSize=std::max(0.0f,rawShadowSize.empty()?8.0f:
            StyleSheet::Length(rawShadowSize,box->rect.height,geometry.bounds.width,8.0f));
        const auto rawShadowColor=box->style.Get(L"--select-menu-shadow-color");
        const unsigned int shadow=StyleSheet::Color(rawShadowColor,0x30000000);
        if(shadowSize>0&&((shadow>>24)&0xff)){
            const unsigned int alpha=(shadow>>24)&0xff;
            const int layers=std::max(1,static_cast<int>(std::ceil(shadowSize)));
            for(int layer=layers;layer>=1;--layer){
                const unsigned int layerAlpha=std::max(1u,alpha*static_cast<unsigned int>(layers-layer+1)/
                    static_cast<unsigned int>(layers*2));
                target->CreateSolidColorBrush(d2d((shadow&0x00ffffffu)|(layerAlpha<<24)),&brush);
                target->FillRectangle(D2D1::RectF(outer.left+1,outer.bottom,outer.right-1,
                    outer.bottom+static_cast<float>(layer)),brush.Get());
            }
        }
        target->CreateSolidColorBrush(d2d(palette.border),&brush);
        target->FillRectangle(outer,brush.Get());
        const auto inner=D2D1::RectF(outer.left+geometry.borderWidth,outer.top+geometry.borderWidth,
            outer.right-geometry.borderWidth,outer.bottom-geometry.borderWidth);
        target->CreateSolidColorBrush(d2d(palette.background),&brush);target->FillRectangle(inner,brush.Get());
        auto family=box->style.Get(L"font-family",L"Segoe UI");const auto comma=family.find(L',');if(comma!=std::wstring::npos)family=Trim(family.substr(0,comma));
        family.erase(std::remove(family.begin(),family.end(),L'\''),family.end());family.erase(std::remove(family.begin(),family.end(),L'"'),family.end());
        int weight=400;try{weight=std::stoi(box->style.Get(L"font-weight",L"400"));}catch(...){if(box->style.Is(L"font-weight",L"bold"))weight=700;}
        const float fontSize=StyleSheet::Length(box->style.Get(L"font-size",L"16px"),16,16,16);
        ComPtr<IDWriteTextFormat> format;
        if(FAILED(writeFactory->CreateTextFormat(family.c_str(),nullptr,static_cast<DWRITE_FONT_WEIGHT>(std::max(1,std::min(999,weight))),
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,fontSize,L"ko-kr",&format)))return;
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextFormat> secondaryFormat;
        if(IsDatalistInput(openSelectPopup)&&SUCCEEDED(writeFactory->CreateTextFormat(family.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,std::max(8.0f,fontSize*0.82f),L"ko-kr",&secondaryFormat))){
            secondaryFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            secondaryFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        const int selected=PopupSelectedIndex(openSelectPopup,options);
        const float contentRight=inner.right-geometry.scrollbarWidth;
        target->PushAxisAlignedClip(D2D1::RectF(inner.left,inner.top,contentRight,inner.bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for(size_t index=0;index<options.size();++index){
            const bool highlighted=static_cast<int>(index)==selectPopupHotIndex||
                (selectPopupHotIndex<0&&static_cast<int>(index)==selected&&openSelectPopup->tag==L"select");
            const float top=inner.top+geometry.rowHeight*static_cast<float>(index)-geometry.scrollOffset;
            const auto row=D2D1::RectF(inner.left,top,contentRight,top+geometry.rowHeight);
            if(row.bottom<=inner.top||row.top>=inner.bottom)continue;
            if(highlighted){target->CreateSolidColorBrush(d2d(palette.selectedBackground),&brush);target->FillRectangle(row,brush.Get());}
            const auto foreground=options[index]->disabled?palette.disabledColor:(highlighted?palette.selectedColor:palette.color);
            target->CreateSolidColorBrush(d2d(foreground),&brush);
            std::wstring label,secondary;
            if(IsDatalistInput(openSelectPopup)){
                label=OptionValue(options[index]);secondary=options[index]->Attribute(L"label");
                if(secondary==label)secondary.clear();
            }else{label=options[index]->Attribute(L"label");if(label.empty())label=Trim(options[index]->InnerText());}
            const auto textRect=D2D1::RectF(row.left+geometry.textInset,row.top,
                row.right-geometry.textInset,secondary.empty()?row.bottom:row.top+geometry.rowHeight*0.62f);
            target->DrawTextW(label.c_str(),static_cast<UINT32>(label.size()),format.Get(),textRect,brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
            if(!secondary.empty()&&secondaryFormat){
                const auto secondaryRect=D2D1::RectF(row.left+geometry.textInset,row.top+geometry.rowHeight*0.48f,
                    row.right-geometry.textInset,row.bottom-2.0f);
                target->DrawTextW(secondary.c_str(),static_cast<UINT32>(secondary.size()),secondaryFormat.Get(),
                    secondaryRect,brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        target->PopAxisAlignedClip();
        if(geometry.scrollbarWidth>0){
            const auto track=D2D1::RectF(contentRight,inner.top,inner.right,inner.bottom);
            target->CreateSolidColorBrush(d2d((palette.border&0x00ffffffu)|0x30000000u),&brush);target->FillRectangle(track,brush.Get());
            const float thumbHeight=std::max(20.0f,geometry.viewportHeight*geometry.viewportHeight/geometry.contentHeight);
            const float maximum=std::max(0.0f,geometry.contentHeight-geometry.viewportHeight);
            const float travel=std::max(0.0f,geometry.viewportHeight-thumbHeight);
            const float thumbTop=inner.top+(maximum>0?geometry.scrollOffset/maximum*travel:0);
            target->CreateSolidColorBrush(d2d((palette.color&0x00ffffffu)|0x66000000u),&brush);
            target->FillRectangle(D2D1::RectF(contentRight+2,thumbTop,inner.right-2,thumbTop+thumbHeight),brush.Get());
        }
    }
    void OpenSelectPopup(const std::shared_ptr<Node>& control,bool showAll=false,bool toggle=true){
        if(!control||control->disabled)return;
        if(openSelectPopup==control&&toggle){CloseSelectPopup();return;}
        openSelectPopup=control;selectPopupShowAll=showAll;selectPopupScrollOffset=0;
        selectPopupHotIndex=control->tag==L"select"?PopupSelectedIndex(control,PopupOptions(control)):-1;
        if(layoutDirty)Rebuild();SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(control,geometry)){CloseSelectPopup();return;}
        EnsureSelectPopupHotVisible();
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void OpenFilePicker(const std::shared_ptr<Node>& input){
        if(!input||input->disabled||input->tag!=L"input"||
           ToLower(input->Attribute(L"type"))!=L"file"||!IsWindowVisible(hwnd))return;
        std::wstring patterns;
        const auto accept=input->Attribute(L"accept");size_t start=0;
        while(start<accept.size()){
            const auto comma=accept.find(L',',start);
            const auto extension=Trim(accept.substr(start,comma==std::wstring::npos?std::wstring::npos:comma-start));
            if(!extension.empty()&&extension.front()==L'.'){
                if(!patterns.empty())patterns+=L';';patterns+=L'*'+extension;
            }
            if(comma==std::wstring::npos)break;start=comma+1;
        }
        if(patterns.empty())patterns=L"*.*";
        std::wstring filter=L"Accepted files";filter+=L'\0';filter+=patterns;filter+=L'\0';
        filter+=L"All files";filter+=L'\0';filter+=L"*.*";filter+=L'\0';filter+=L'\0';
        std::vector<wchar_t> path(32768,L'\0');
        OPENFILENAMEW options{sizeof(options)};options.hwndOwner=GetAncestor(hwnd,GA_ROOT);
        options.lpstrFile=path.data();options.nMaxFile=static_cast<DWORD>(path.size());
        options.lpstrFilter=filter.c_str();options.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&options))return;
        Node::FileInfo info;
        if(!FileInfoForPath(path.data(),info))return;
        input->files={info};
        input->SetAttribute(L"value",L"C:\\fakepath\\"+info.name);
        javascript.DispatchNodeEvent(input,L"input");javascript.DispatchNodeEvent(input,L"change");
        layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);
    }
    void Activate(const std::shared_ptr<Node>& node,float clickX,float clickY,bool keyboardFocusVisible=false){if(node&&(node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true"))return;SetFocusedNode(node,keyboardFocusVisible,false);if(!node){layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);return;}const auto focusTarget=FocusTarget(node);auto type=ToLower(node->Attribute(L"type"));const auto role=ToLower(node->Attribute(L"role"));bool changed=false;
        if(node->tag==L"input"&&type==L"checkbox"&&!node->disabled){node->indeterminate=false;node->checked=!node->checked;changed=true;}
        else if(node->tag==L"input"&&type==L"radio"&&!node->disabled){auto name=node->Attribute(L"name");for(auto& other:document.GetElementsByName(name))other->checked=(other==node);changed=true;}
        else if(role==L"checkbox"||role==L"switch"){const auto value=ToLower(node->Attribute(L"aria-checked"));node->SetAttribute(L"aria-checked",value==L"true"?L"false":L"true");changed=true;}
        else if(role==L"radio"){node->SetAttribute(L"aria-checked",L"true");changed=true;}
        const bool clickCanceled=javascript.DispatchNodeEvent(node,L"click");
        if(changed)javascript.DispatchNodeEvent(node,L"change");
        if(!clickCanceled){
            const auto anchor=node->Closest(L"a[href]");
            if(anchor){const auto href=Trim(anchor->Attribute(L"href"));
                if(!href.empty()&&href.front()==L'#')javascript.NavigateToFragment(href);
            }
        }
        layoutDirty=true;
        if(IsTextControl(focusTarget)||IsContentEditable(focusTarget)){
            BeginEditingAt(node,clickX,clickY);
            if(IsDatalistInput(focusTarget)){
                const auto* box=layout.BoxFor(focusTarget);
                const bool indicator=box&&clickX>=box->rect.x+box->rect.width-box->rect.height;
                OpenSelectPopup(focusTarget,indicator,false);
            }
        }
        else if(node->tag==L"select")OpenSelectPopup(node);
        else if(node->tag==L"input"&&ToLower(type)==L"file")OpenFilePicker(node);
        else if(auto label=node->Closest(L"label")){
            auto control=document.GetElementById(label->Attribute(L"for"));
            if(!control)control=document.QuerySelector(L"input",label);
            if(control&&control->tag==L"input"&&ToLower(control->Attribute(L"type"))==L"file"){
                SetFocusedNode(control,false,false);OpenFilePicker(control);
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    void RecordUndo(){
        if(!editingNode)return;
        undoHistory.push_back({EditingValue(),selectionAnchor,caretPosition});
        if(undoHistory.size()>512)undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    bool ApplyHistory(bool redo){
        auto& source=redo?redoHistory:undoHistory;
        auto& destination=redo?undoHistory:redoHistory;
        if(!CanEditText()||source.empty())return false;
        JavaScriptRuntime::EventInit before{};
        before.inputType=redo?L"historyRedo":L"historyUndo";
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return false;
        destination.push_back({EditingValue(),selectionAnchor,caretPosition});
        const auto snapshot=std::move(source.back());source.pop_back();
        SetEditingValue(snapshot.value);selectionAnchor=std::min(snapshot.anchor,snapshot.value.size());
        caretPosition=std::min(snapshot.caret,snapshot.value.size());StoreControlSelection();
        if(IsTextControl(focused))textEditDirty=true;
        javascript.DispatchNodeEvent(focused,L"input",before);layoutDirty=true;
        ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    bool CopySelectionToClipboard(){
        if(!editingNode)return false;const auto value=EditingValue();
        const size_t begin=std::min({selectionAnchor,caretPosition,value.size()});
        const size_t end=std::min(value.size(),std::max(selectionAnchor,caretPosition));
        if(begin==end||!OpenClipboard(hwnd))return false;
        const auto selected=value.substr(begin,end-begin);
        const SIZE_T bytes=(selected.size()+1)*sizeof(wchar_t);
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!memory){CloseClipboard();return false;}
        void* target=GlobalLock(memory);if(!target){GlobalFree(memory);CloseClipboard();return false;}
        std::memcpy(target,selected.c_str(),bytes);GlobalUnlock(memory);
        EmptyClipboard();
        if(!SetClipboardData(CF_UNICODETEXT,memory)){GlobalFree(memory);CloseClipboard();return false;}
        CloseClipboard();return true;
    }
    bool PasteFromClipboard(){
        if(!CanEditText()||!OpenClipboard(hwnd))return false;
        HANDLE data=GetClipboardData(CF_UNICODETEXT);if(!data){CloseClipboard();return false;}
        const auto* text=static_cast<const wchar_t*>(GlobalLock(data));
        if(!text){CloseClipboard();return false;}
        std::wstring value(text);GlobalUnlock(data);CloseClipboard();
        if(IsTextInput(focused)){
            value.erase(std::remove(value.begin(),value.end(),L'\r'),value.end());
            const auto lineBreak=value.find(L'\n');if(lineBreak!=std::wstring::npos)value.resize(lineBreak);
        }else{
            size_t position=0;while((position=value.find(L"\r\n",position))!=std::wstring::npos)value.replace(position,2,L"\n");
            std::replace(value.begin(),value.end(),L'\r',L'\n');
        }
        return ReplaceSelection(value,L"insertFromPaste");
    }
    bool ReplaceSelection(const std::wstring& replacement,const std::wstring& inputType=L"insertText",
                           bool isComposing=false){
        if(!CanEditText())return false;auto value=EditingValue();size_t begin=std::min(selectionAnchor,caretPosition),end=std::min(value.size(),std::max(selectionAnchor,caretPosition));begin=std::min(begin,value.size());std::wstring inserted=replacement;
        if(IsTextControl(focused)){const auto maximum=focused->Attribute(L"maxlength");if(!maximum.empty()){try{const size_t limit=std::stoul(maximum);const size_t kept=value.size()-(end-begin);if(kept>=limit)inserted.clear();else if(inserted.size()>limit-kept)inserted.resize(limit-kept);}catch(...){} }}
        if(begin==end&&inserted.empty())return false;
        JavaScriptRuntime::EventInit before{};before.data=inserted;before.inputType=inputType;before.isComposing=isComposing;
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return false;
        RecordUndo();
        value.replace(begin,end-begin,inserted);selectionAnchor=caretPosition=begin+inserted.size();SetEditingValue(value);
        StoreControlSelection();
        if(IsTextControl(focused))textEditDirty=true;
        JavaScriptRuntime::EventInit input=before;javascript.DispatchNodeEvent(focused,L"input",input);
        if(openSelectPopup==focused&&IsDatalistInput(focused)){
            selectPopupShowAll=false;selectPopupHotIndex=-1;selectPopupScrollOffset=0;
            if(PopupOptions(focused).empty())CloseSelectPopup();
        }
        layoutDirty=true;ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    bool Backspace(){
        if(!CanEditText())return false;const auto value=EditingValue();if(selectionAnchor==caretPosition){if(caretPosition==0)return false;selectionAnchor=PreviousTextPosition(value,std::min(caretPosition,value.size()));}return ReplaceSelection(L"",L"deleteContentBackward");
    }
    bool DeleteForward(){
        if(!CanEditText())return false;const auto value=EditingValue();if(selectionAnchor==caretPosition){if(caretPosition>=value.size())return false;selectionAnchor=NextTextPosition(value,caretPosition);}return ReplaceSelection(L"",L"deleteContentForward");
    }
    void MoveCaret(size_t position,bool extend){
        if(!editingNode)return;const auto length=EditingValue().size();caretPosition=std::min(position,length);if(!extend)selectionAnchor=caretPosition;StoreControlSelection();textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();
    }
    HCURSOR CursorAtCurrentPosition(){
        POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);if(layoutDirty)Rebuild();
        const auto node=layout.HitTest(PixelToDip(static_cast<float>(point.x)),PixelToDip(static_cast<float>(point.y)));
        std::wstring cursor;if(node)if(const auto* box=layout.BoxFor(node))cursor=ToLower(box->style.Get(L"cursor",L"auto"));
        if(cursor==L"ns-resize"||cursor==L"n-resize"||cursor==L"s-resize"||cursor==L"row-resize")return LoadCursorW(nullptr,IDC_SIZENS);
        if(cursor==L"ew-resize"||cursor==L"e-resize"||cursor==L"w-resize"||cursor==L"col-resize")return LoadCursorW(nullptr,IDC_SIZEWE);
        if(cursor==L"nwse-resize"||cursor==L"nw-resize"||cursor==L"se-resize")return LoadCursorW(nullptr,IDC_SIZENWSE);
        if(cursor==L"nesw-resize"||cursor==L"ne-resize"||cursor==L"sw-resize")return LoadCursorW(nullptr,IDC_SIZENESW);
        if(cursor==L"move"||cursor==L"all-scroll")return LoadCursorW(nullptr,IDC_SIZEALL);
        if(cursor==L"crosshair")return LoadCursorW(nullptr,IDC_CROSS);
        if(cursor==L"not-allowed"||cursor==L"no-drop")return LoadCursorW(nullptr,IDC_NO);
        if(cursor==L"wait")return LoadCursorW(nullptr,IDC_WAIT);
        if(cursor==L"progress")return LoadCursorW(nullptr,IDC_APPSTARTING);
        if(cursor==L"text")return LoadCursorW(nullptr,IDC_IBEAM);
        if(cursor==L"pointer")return LoadCursorW(nullptr,IDC_HAND);
        return LoadCursorW(nullptr,(IsTextControl(node)||EditableRoot(node))?IDC_IBEAM:
            (node&&(node->tag==L"button"||node->tag==L"select"||!node->Attribute(L"onclick").empty())?IDC_HAND:IDC_ARROW));
    }
    LRESULT ParentResizeHit(POINT screenPoint)const{
        const HWND parent=GetParent(hwnd);if(!parent)return HTCLIENT;
        return SendMessageW(parent,WM_NCHITTEST,0,
            MAKELPARAM(static_cast<WORD>(screenPoint.x),static_cast<WORD>(screenPoint.y)));
    }
    static HCURSOR ResizeCursor(LRESULT hit){
        if(hit==HTLEFT||hit==HTRIGHT)return LoadCursorW(nullptr,IDC_SIZEWE);
        if(hit==HTTOP||hit==HTBOTTOM)return LoadCursorW(nullptr,IDC_SIZENS);
        if(hit==HTTOPLEFT||hit==HTBOTTOMRIGHT)return LoadCursorW(nullptr,IDC_SIZENWSE);
        if(hit==HTTOPRIGHT||hit==HTBOTTOMLEFT)return LoadCursorW(nullptr,IDC_SIZENESW);
        return nullptr;
    }
    LRESULT HandleMessage(UINT message,WPARAM wParam,LPARAM lParam){
        LRESULT textResult=0;if(textInput.HandleMessage(hwnd,message,wParam,lParam,textResult))return textResult;
        switch(message){
        case kAccessibilityDispatchMessage:return accessibility?accessibility->HandleDispatch(lParam):0;
        case WM_GETOBJECT:if(lParam==static_cast<LPARAM>(UiaRootObjectId)&&accessibility)return accessibility->ReturnRawProvider(wParam,lParam);break;
        case WM_NCHITTEST:{
            const HWND parent=GetParent(hwnd);if(parent){const LRESULT hit=SendMessageW(parent,WM_NCHITTEST,wParam,lParam);
                // Child HWND non-client resize results are not promoted to the
                // top-level sizing loop. Keep the child hit client-owned; the
                // cursor and button handlers below explicitly route the same
                // parent hit to the top-level window.
                if(hit>=HTLEFT&&hit<=HTBOTTOMRIGHT)return HTCLIENT;}
            break;
        }
        case WM_NCLBUTTONDOWN:
            if(wParam>=HTLEFT&&wParam<=HTBOTTOMRIGHT){const HWND parent=GetParent(hwnd);if(parent){ReleaseCapture();return SendMessageW(parent,message,wParam,lParam);}}
            break;
        case WM_PAINT:Paint();return 0;case WM_ERASEBKGND:return 1;
        case WM_IME_SETCONTEXT:return DefWindowProcW(hwnd,message,wParam,lParam&~ISC_SHOWUICOMPOSITIONWINDOW);
        case WM_SETFOCUS:textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();return 0;
        case WM_TIMER:if(wParam==kAnimationFrameTimer){KillTimer(hwnd,kAnimationFrameTimer);javascript.RunAnimationFrame();return 0;}if(wParam==kJavaScriptTimer){KillTimer(hwnd,kJavaScriptTimer);javascript.RunTimers();return 0;}if(wParam==kCssTransitionTimer){const auto now=std::chrono::steady_clock::now();const float elapsed=std::max(1.0f,std::chrono::duration<float,std::milli>(now-cssTransitionTick).count());cssTransitionTick=now;if(layoutDirty)Rebuild();const bool remains=layout.AdvanceTransitions(elapsed);UpdateFrameBounds();InvalidateRect(hwnd,nullptr,FALSE);if(!remains){KillTimer(hwnd,kCssTransitionTimer);cssTransitionTimerActive=false;}return 0;}if(wParam==kCaretBlinkTimer){if(CanBlinkCaret()){caretVisible=!caretVisible;InvalidateCaret();}else StopCaretBlink();return 0;}break;
        case kAnimationFrameFallbackMessage:javascript.RunAnimationFrame();return 0;
        case WM_SIZE:{const bool viewportOnly=!layoutDirty||viewportOnlyDirty;layoutDirty=true;viewportOnlyDirty=viewportOnly;UpdateJavaScriptViewport();javascript.DispatchWindowEvent(L"resize");InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        case WM_DPICHANGED:{const bool viewportOnly=!layoutDirty||viewportOnlyDirty;layoutDirty=true;viewportOnlyDirty=viewportOnly;UpdateJavaScriptViewport();javascript.DispatchWindowEvent(L"resize");InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        case WM_DROPFILES:{
            const auto drop=reinterpret_cast<HDROP>(wParam);
            POINT point{};DragQueryPoint(drop,&point);
            std::vector<Node::FileInfo> files;
            const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
            for(UINT index=0;index<count;++index){
                const UINT length=DragQueryFileW(drop,index,nullptr,0);
                std::wstring path(length+1,L'\0');
                DragQueryFileW(drop,index,path.data(),length+1);path.resize(length);
                Node::FileInfo info;if(FileInfoForPath(path,info))files.push_back(std::move(info));
            }
            DragFinish(drop);
            if(!files.empty()){
                if(layoutDirty)Rebuild();
                javascript.DispatchFileDrop(layout.HitTest(PixelToDip(static_cast<float>(point.x)),
                                                       PixelToDip(static_cast<float>(point.y))),files);
                layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:{
            POINT screenPoint{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ClientToScreen(hwnd,&screenPoint);
            const LRESULT resizeHit=ParentResizeHit(screenPoint);if(resizeHit>=HTLEFT&&resizeHit<=HTBOTTOMRIGHT){ReleaseCapture();PostMessageW(GetParent(hwnd),WM_NCLBUTTONDOWN,static_cast<WPARAM>(resizeHit),MAKELPARAM(static_cast<WORD>(screenPoint.x),static_cast<WORD>(screenPoint.y)));return 0;}
            const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));if(layoutDirty)Rebuild();
            if(openSelectPopup){
                SelectPopupGeometry popup;const auto control=openSelectPopup;const auto* controlBox=layout.BoxFor(control);
                const int optionIndex=SelectPopupIndexAt(x,y);
                if(optionIndex>=0){const auto options=PopupOptions(control);if(static_cast<size_t>(optionIndex)<options.size()&&!options[optionIndex]->disabled)ChoosePopupOption(control,static_cast<size_t>(optionIndex));CloseSelectPopup();return 0;}
                if(GetSelectPopupGeometry(control,popup)&&popup.bounds.Contains(x,y)){
                    const float scrollbarLeft=popup.bounds.x+popup.bounds.width-popup.borderWidth-popup.scrollbarWidth;
                    if(popup.scrollbarWidth>0&&x>=scrollbarLeft){
                        const float innerTop=popup.bounds.y+popup.borderWidth;
                        const float thumbHeight=std::max(20.0f,popup.viewportHeight*popup.viewportHeight/popup.contentHeight);
                        const float maximum=std::max(0.0f,popup.contentHeight-popup.viewportHeight);
                        const float travel=std::max(0.0f,popup.viewportHeight-thumbHeight);
                        const float thumbTop=innerTop+(maximum>0?popup.scrollOffset/maximum*travel:0);
                        if(y>=thumbTop&&y<=thumbTop+thumbHeight){selectPopupScrollDragging=true;selectPopupScrollDragOffset=y-thumbTop;SetCapture(hwnd);}
                        else ScrollSelectPopup(y<thumbTop?-popup.viewportHeight*0.9f:popup.viewportHeight*0.9f);
                    }
                    return 0;
                }
                if(controlBox&&controlBox->rect.Contains(x,y)){
                    if(control->tag==L"select")CloseSelectPopup();
                    else OpenSelectPopup(control,x>=controlBox->rect.x+controlBox->rect.width-controlBox->rect.height,false);
                    return 0;
                }
                CloseSelectPopup();
            }
            if(layout.BeginScrollbarInteraction(x,y,scrollbarDragNode,scrollbarDragOffset,scrollbarDragHorizontal)){if(scrollbarDragNode)SetCapture(hwnd);else javascript.DispatchNodeEvent(layout.HitTest(x,y),L"scroll");if(accessibility)accessibility->Invalidate();InvalidateRect(hwnd,nullptr,FALSE);return 0;}
            SetFocus(hwnd);auto target=layout.HitTest(x,y);JavaScriptRuntime::EventInit pointer{};pointer.button=0;pointer.detail=1;if(javascript.DispatchNodeEvent(target,L"pointerdown",pointer))return 0;Activate(target,x,y);
            if(editingNode&&focused&&FocusTarget(target)==focused&&
               (IsTextControl(focused)||IsContentEditable(focused))){
                textSelectionDragging=true;SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:{const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));if(layoutDirty)Rebuild();auto target=layout.HitTest(x,y);JavaScriptRuntime::EventInit pointer{};pointer.button=0;pointer.detail=2;javascript.DispatchNodeEvent(target,L"pointerdown",pointer);javascript.DispatchNodeEvent(target,L"dblclick",pointer);return 0;}
        case WM_LBUTTONUP:
            if(selectPopupScrollDragging){selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;if(GetCapture()==hwnd)ReleaseCapture();return 0;}
            if(scrollbarDragNode){InvalidateRect(hwnd,nullptr,FALSE);scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;if(GetCapture()==hwnd)ReleaseCapture();return 0;}
            if(textSelectionDragging){
                const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam)));
                const float y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));
                UpdateTextSelectionAt(x,y);textSelectionDragging=false;
                if(GetCapture()==hwnd)ReleaseCapture();return 0;
            }
            break;
        case WM_CAPTURECHANGED:textSelectionDragging=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;return 0;
        case WM_MOUSEWHEEL:{
            if(layoutDirty)Rebuild();POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(hwnd,&point);
            const float x=PixelToDip(static_cast<float>(point.x)),y=PixelToDip(static_cast<float>(point.y));
            if(openSelectPopup){SelectPopupGeometry popup;if(GetSelectPopupGeometry(openSelectPopup,popup)&&popup.bounds.Contains(x,y)){ScrollSelectPopup(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam))/WHEEL_DELTA*popup.rowHeight*3.0f);return 0;}CloseSelectPopup();}
            std::shared_ptr<Node> scrolled;if(layout.ScrollAt(x,y,static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)),&scrolled)){javascript.DispatchNodeEvent(scrolled,L"scroll");if(accessibility)accessibility->Invalidate();textInput.UpdateCandidateWindow(hwnd);InvalidateRect(hwnd,nullptr,FALSE);}return 0;
        }
        case WM_MOUSEHWHEEL:{
            if(layoutDirty)Rebuild();POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(hwnd,&point);
            const float x=PixelToDip(static_cast<float>(point.x)),y=PixelToDip(static_cast<float>(point.y));
            std::shared_ptr<Node> scrolled;if(layout.ScrollAt(x,y,-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)),&scrolled,true)){javascript.DispatchNodeEvent(scrolled,L"scroll");if(accessibility)accessibility->Invalidate();textInput.UpdateCandidateWindow(hwnd);InvalidateRect(hwnd,nullptr,FALSE);}return 0;
        }
        case WM_MOUSEMOVE:{
            if(!trackingMouseLeave){TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT),TME_LEAVE,hwnd,0};trackingMouseLeave=TrackMouseEvent(&tracking)!=FALSE;}
            const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));
            if(selectPopupScrollDragging){SelectPopupGeometry popup;if(GetSelectPopupGeometry(openSelectPopup,popup)){
                const float thumbHeight=std::max(20.0f,popup.viewportHeight*popup.viewportHeight/popup.contentHeight);
                const float travel=std::max(0.0f,popup.viewportHeight-thumbHeight),maximum=std::max(0.0f,popup.contentHeight-popup.viewportHeight);
                const float thumbTop=std::max(0.0f,std::min(travel,y-(popup.bounds.y+popup.borderWidth)-selectPopupScrollDragOffset));
                const float previous=selectPopupScrollOffset;selectPopupScrollOffset=travel>0?thumbTop/travel*maximum:0;
                if(std::abs(previous-selectPopupScrollOffset)>0.01f)InvalidateRect(hwnd,nullptr,FALSE);
            }return 0;}
            if(scrollbarDragNode){if(layout.DragScrollbar(scrollbarDragNode,x,y,scrollbarDragOffset,scrollbarDragHorizontal)){javascript.DispatchNodeEvent(scrollbarDragNode,L"scroll");if(accessibility)accessibility->Invalidate();textInput.UpdateCandidateWindow(hwnd);InvalidateRect(hwnd,nullptr,FALSE);}return 0;}
            if(textSelectionDragging){UpdateTextSelectionAt(x,y);return 0;}
            if(layoutDirty)Rebuild();if(openSelectPopup){int hot=SelectPopupIndexAt(x,y);const auto options=PopupOptions(openSelectPopup);if(hot>=0&&(static_cast<size_t>(hot)>=options.size()||options[hot]->disabled))hot=-1;if(hot!=selectPopupHotIndex){selectPopupHotIndex=hot;InvalidateRect(hwnd,nullptr,FALSE);}SelectPopupGeometry popup;if(GetSelectPopupGeometry(openSelectPopup,popup)&&popup.bounds.Contains(x,y))return 0;}
            auto n=layout.HitTest(x,y);if(n!=hovered){LayoutRect dirty{};bool targeted=false;layoutDirty=SetHoveredNode(n,&dirty,&targeted);if(!layoutDirty&&targeted)InvalidateDipBounds(dirty);else InvalidateRect(hwnd,nullptr,FALSE);}return 0;
        }
        case WM_MOUSELEAVE:trackingMouseLeave=false;if(hovered){LayoutRect dirty{};bool targeted=false;layoutDirty=SetHoveredNode({},&dirty,&targeted);if(!layoutDirty&&targeted)InvalidateDipBounds(dirty);else InvalidateRect(hwnd,nullptr,FALSE);}return 0;
        case WM_GETDLGCODE:return DLGC_WANTTAB|DLGC_WANTARROWS|
            ((CanEditText()||focused&&focused->tag==L"select")?DLGC_WANTCHARS:0);
        case WM_KEYDOWN:{JavaScriptRuntime::EventInit key{};key.key=KeyValue(wParam);key.ctrlKey=(GetKeyState(VK_CONTROL)&0x8000)!=0;key.shiftKey=(GetKeyState(VK_SHIFT)&0x8000)!=0;key.altKey=(GetKeyState(VK_MENU)&0x8000)!=0;key.metaKey=(GetKeyState(VK_LWIN)&0x8000)!=0||(GetKeyState(VK_RWIN)&0x8000)!=0;if(javascript.DispatchNodeEvent(focused,L"keydown",key))return 0;}
        if(textInput.IsComposing())break;
        if(openSelectPopup){
            if(wParam==VK_ESCAPE||wParam==VK_F4){CloseSelectPopup();return 0;}
            if(wParam==VK_UP){MoveSelectPopupHot(-1);return 0;}
            if(wParam==VK_DOWN){MoveSelectPopupHot(1);return 0;}
            if(wParam==VK_HOME){MoveSelectPopupHot(-1,true);return 0;}
            if(wParam==VK_END){MoveSelectPopupHot(1,true);return 0;}
            if(wParam==VK_SPACE||wParam==VK_RETURN){const auto control=openSelectPopup;const int option=selectPopupHotIndex;if(option>=0)ChoosePopupOption(control,static_cast<size_t>(option));CloseSelectPopup();return 0;}
            if(wParam==VK_TAB)CloseSelectPopup();
        }
        if(wParam==VK_TAB){MoveSequentialFocus((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}
        if(MoveMenuFocus(wParam))return 0;
        if(focused&&focused->tag==L"select"){
            const bool alt=(GetKeyState(VK_MENU)&0x8000)!=0;
            if(wParam==VK_F4||(alt&&wParam==VK_DOWN)||wParam==VK_SPACE||wParam==VK_RETURN){OpenSelectPopup(focused);return 0;}
            if(wParam==VK_UP){MoveSelectOption(focused,-1);return 0;}
            if(wParam==VK_DOWN){MoveSelectOption(focused,1);return 0;}
            if(wParam==VK_HOME){MoveSelectOption(focused,-1,true);return 0;}
            if(wParam==VK_END){MoveSelectOption(focused,1,true);return 0;}
        }
        if(IsKeyboardActivatable(focused)&&(wParam==VK_SPACE||wParam==VK_RETURN)){
            if(layoutDirty)Rebuild();float x=0,y=0;if(const auto* box=layout.BoxFor(focused)){
                x=box->content.x+box->content.width/2.0f;y=box->content.y+box->content.height/2.0f;
            }
            Activate(focused,x,y,true);return 0;
        }
        if(CanEditText()){
            const bool extend=(GetKeyState(VK_SHIFT)&0x8000)!=0,control=(GetKeyState(VK_CONTROL)&0x8000)!=0;const auto value=EditingValue();
            if(control&&(wParam==L'C'||wParam==L'c')){CopySelectionToClipboard();return 0;}
            if(control&&(wParam==L'X'||wParam==L'x')){if(CopySelectionToClipboard())ReplaceSelection(L"",L"deleteByCut");return 0;}
            if(control&&(wParam==L'V'||wParam==L'v')){PasteFromClipboard();return 0;}
            if(control&&(wParam==L'Z'||wParam==L'z')){ApplyHistory(extend);return 0;}
            if(control&&(wParam==L'Y'||wParam==L'y')){ApplyHistory(true);return 0;}
            if(control&&(wParam==L'A'||wParam==L'a')){selectionAnchor=0;caretPosition=value.size();StoreControlSelection();ResetCaretBlink();return 0;}
            if(wParam==VK_LEFT){MoveCaret(caretPosition==selectionAnchor?PreviousTextPosition(value,caretPosition):std::min(caretPosition,selectionAnchor),extend);return 0;}
            if(wParam==VK_RIGHT){MoveCaret(caretPosition==selectionAnchor?NextTextPosition(value,caretPosition):std::max(caretPosition,selectionAnchor),extend);return 0;}
            if(wParam==VK_HOME){MoveCaret(0,extend);return 0;}if(wParam==VK_END){MoveCaret(value.size(),extend);return 0;}if(wParam==VK_DELETE){DeleteForward();return 0;}if(wParam==VK_RETURN&&IsTextInput(focused))return 0;
        }break;
        case WM_COPY:CopySelectionToClipboard();return 0;
        case WM_CUT:if(CopySelectionToClipboard())ReplaceSelection(L"",L"deleteByCut");return 0;
        case WM_PASTE:PasteFromClipboard();return 0;
        case WM_UNDO:ApplyHistory(false);return 0;
        case WM_CHAR:if(textInput.IsComposing())return 0;else if(CanEditText()){if(wParam==VK_BACK)Backspace();else if(wParam==L'\r'){if(!IsTextInput(focused))ReplaceSelection(L"\n",L"insertLineBreak");}else if(wParam>=32&&wParam!=127)ReplaceSelection(std::wstring(1,static_cast<wchar_t>(wParam)));return 0;}break;
        case WM_UNICHAR:if(wParam==UNICODE_NOCHAR)return TRUE;else if(CanEditText()&&wParam>=32&&wParam<=0x10ffff){std::wstring text;if(wParam<=0xffff)text.push_back(static_cast<wchar_t>(wParam));else{const auto value=static_cast<unsigned>(wParam)-0x10000;text.push_back(static_cast<wchar_t>(0xd800+(value>>10)));text.push_back(static_cast<wchar_t>(0xdc00+(value&0x3ff)));}ReplaceSelection(text);return 0;}break;
        case WM_KILLFOCUS:textSelectionDragging=false;if(GetCapture()==hwnd)ReleaseCapture();CloseSelectPopup();textInput.Cancel(hwnd);SetFocusedNode({});StopCaretBlink();InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_SETCURSOR:{POINT screenPoint{};GetCursorPos(&screenPoint);const LRESULT parentHit=ParentResizeHit(screenPoint);if(const HCURSOR resize=ResizeCursor(parentHit)){SetCursor(resize);return TRUE;}const UINT hit=LOWORD(lParam);
            if(const HCURSOR resize=ResizeCursor(hit)){SetCursor(resize);return TRUE;}
            if(hit==HTCLIENT){SetCursor(CursorAtCurrentPosition());return TRUE;}break;}
        case WM_DESTROY:KillTimer(hwnd,kAnimationFrameTimer);KillTimer(hwnd,kJavaScriptTimer);KillTimer(hwnd,kCssTransitionTimer);KillTimer(hwnd,kCaretBlinkTimer);caretBlinkTimerActive=false;cssTransitionTimerActive=false;childFrames.clear();textInput.Cancel(nullptr);if(accessibility)accessibility->Disconnect();ResetRenderTargets();return 0;default:break;}return DefWindowProcW(hwnd,message,wParam,lParam);}
    bool LoadHtml(const std::wstring& html,const std::wstring& base,const std::wstring& location){
        lastError.clear();childFrames.clear();basePath=base;textInput.Cancel(nullptr);focused.reset();editingNode.reset();textSelectionDragging=false;if(GetCapture()==hwnd)ReleaseCapture();StopCaretBlink();hovered.reset();hoverPath.clear();scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;openSelectPopup.reset();selectPopupHotIndex=-1;selectPopupScrollOffset=0;selectPopupShowAll=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;selectionAnchor=caretPosition=0;textEditDirty=false;liveRegions.clear();liveRegionText.clear();KillTimer(hwnd,kCssTransitionTimer);cssTransitionTimerActive=false;layout.ClearTransitions();javascript.Clear();UpdateJavaScriptViewport();if(!document.Parse(html,&lastError)){if(loadHandler)loadHandler(false,lastError);return false;}
        std::wstring css=document.StyleText();
        for(const auto& link:document.QuerySelectorAll(L"link[rel='stylesheet']")){
            std::wstring external;if(LoadTextResource(link->Attribute(L"href"),external))css+=external+L"\n";
            else{lastError=L"Cannot load stylesheet: "+link->Attribute(L"href");if(loadHandler)loadHandler(false,lastError);return false;}
        }
        if(!styles.Parse(css,&lastError)){if(loadHandler)loadHandler(false,lastError);return false;}
        std::wstring scripts;
        for(const auto& script:document.QuerySelectorAll(L"script")){
            const auto source=script->Attribute(L"src");
            if(source.empty())scripts+=script->InnerText()+L"\n";
            else{std::wstring external;if(LoadTextResource(source,external))scripts+=external+L"\n";else{lastError=L"Cannot load script: "+source;if(loadHandler)loadHandler(false,lastError);return false;}}
        }
        javascript.SetLocation(location);
        if(!javascript.Load(scripts,&lastError)){if(loadHandler)loadHandler(false,lastError);return false;}
        javascript.DispatchDocumentEvent(L"DOMContentLoaded");
        for(const auto& node:document.QuerySelectorAll(L"iframe[src]")){
            const auto source=node->Attribute(L"src");std::wstring childHtml;
            if(!LoadTextResource(source,childHtml)){lastError=L"Cannot load frame: "+source;if(loadHandler)loadHandler(false,lastError);return false;}
            RECT bounds{0,0,1,1};auto child=View::Create(hwnd,bounds);
            if(!child){lastError=L"Cannot create frame: "+source;if(loadHandler)loadHandler(false,lastError);return false;}
            child->SetResourceLoader(resourceLoader);
            child->SetMessageHandler([this](const std::wstring& message){if(messageHandler)messageHandler(message);});
            child->impl_->javascript.SetParentMessageSink([this,node](const std::wstring& data){
                javascript.DispatchWindowMessageAsJson(data,node);
                layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);
            });
            childFrames.push_back({node,std::move(child)});
            auto framePath=std::filesystem::path(source);const auto suffix=source.find_first_of(L"?#");
            if(suffix!=std::wstring::npos)framePath=std::filesystem::path(source.substr(0,suffix));
            if(framePath.is_relative())framePath=std::filesystem::path(basePath)/framePath;
            auto& frame=*childFrames.back().view;
            if(!frame.impl_->LoadHtml(childHtml,framePath.parent_path().wstring(),framePath.wstring())){
                lastError=L"Cannot load frame: "+source+L" ("+frame.LastError()+L")";
                if(loadHandler)loadHandler(false,lastError);return false;
            }
            javascript.DispatchNodeEvent(node,L"load");
        }
        layoutDirty=true;InvalidateRect(hwnd,nullptr,FALSE);JavaScriptRuntime::Mutation initialMutation;
        initialMutation.kind=JavaScriptRuntime::MutationKind::Tree;initialMutation.targets.push_back(document.Root());
        NotifyAccessibilityMutation(initialMutation,true);if(loadHandler)loadHandler(true,L"");return true;
    }
};

View::View(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
View::~View(){
    // Disconnect UI Automation before DestroyWindow begins its synchronous
    // message teardown. UiaDisconnectProvider cannot make the required COM
    // call while handling a SendMessage-delivered WM_DESTROY.
    if(impl_&&impl_->accessibility)impl_->accessibility->Disconnect();
    if(impl_&&impl_->hwnd&&IsWindow(impl_->hwnd))DestroyWindow(impl_->hwnd);
}
std::unique_ptr<View> View::Create(HWND parent,const RECT& bounds){auto impl=std::make_unique<Impl>();if(!impl->Initialize(parent,bounds))return {};return std::unique_ptr<View>(new View(std::move(impl)));}
HWND View::Window()const noexcept{return impl_->hwnd;}
void View::SetBounds(const RECT& bounds){MoveWindow(impl_->hwnd,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,TRUE);}
void View::SetVisible(bool visible){ShowWindow(impl_->hwnd,visible?SW_SHOW:SW_HIDE);}
void View::SetMessageHandler(MessageHandler handler){impl_->messageHandler=std::move(handler);}
void View::SetLoadHandler(LoadHandler handler){impl_->loadHandler=std::move(handler);}
void View::SetResourceLoader(ResourceLoader loader){impl_->resourceLoader=std::move(loader);}
bool View::Navigate(const std::wstring& filePath){auto path=filePath;const auto suffix=path.find_first_of(L"?#");if(suffix!=std::wstring::npos)path.resize(suffix);std::ifstream input(path,std::ios::binary);if(!input){impl_->lastError=L"Cannot open HTML file: "+path;if(impl_->loadHandler)impl_->loadHandler(false,impl_->lastError);return false;}std::ostringstream bytes;bytes<<input.rdbuf();auto slash=path.find_last_of(L"\\/");return impl_->LoadHtml(Utf8ToWide(bytes.str()),slash==std::wstring::npos?L"":path.substr(0,slash),filePath);}
bool View::NavigateToString(const std::wstring& html,const std::wstring& basePath){return impl_->LoadHtml(html,basePath,basePath);}
bool View::ExecuteScript(const std::wstring& source,std::wstring* result,std::wstring* error){const bool ok=impl_->javascript.Execute(source,result,error);if(!ok)impl_->lastError=error?*error:L"JavaScript execution failed";impl_->RefreshTextSelectionFromDom();return ok;}
bool View::PostWebMessageAsJson(const std::wstring& json,std::wstring* error){const bool ok=impl_->javascript.DispatchWebMessageAsJson(json,error);if(!ok)impl_->lastError=error?*error:L"Invalid JSON web message";impl_->RefreshTextSelectionFromDom();return ok;}
void View::PostWebMessageAsString(const std::wstring& message){impl_->javascript.DispatchWebMessageAsString(message);impl_->RefreshTextSelectionFromDom();}
std::wstring View::DumpLayoutJson()const{if(impl_->layoutDirty)const_cast<Impl*>(impl_.get())->Rebuild();return impl_->layout.DumpJson();}
std::wstring View::DumpAccessibilityJson()const{return const_cast<Impl*>(impl_.get())->DumpAccessibilityJson();}
std::wstring View::LastError()const{return impl_->lastError;}

} // namespace TWebFrame
