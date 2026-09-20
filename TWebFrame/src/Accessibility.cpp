#include "Accessibility.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <oleauto.h>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")

namespace TWebFrame::Internal {
namespace {

class AccessibilityProvider final : public IRawElementProviderSimple,
                                    public IRawElementProviderFragment,
                                    public IRawElementProviderFragmentRoot,
                                    public IInvokeProvider,
                                    public IValueProvider,
                                    public ISelectionProvider,
                                    public ISelectionItemProvider,
                                    public IToggleProvider,
                                    public IExpandCollapseProvider {
public:
    AccessibilityProvider(std::shared_ptr<AccessibilityHost> host,
                          std::shared_ptr<Node> node, bool root = false)
        : host_(std::move(host)), node_(std::move(node)), root_(root) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if(!object)return E_INVALIDARG;*object=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IRawElementProviderSimple))
            *object=static_cast<IRawElementProviderSimple*>(this);
        else if(iid==__uuidof(IRawElementProviderFragment))
            *object=static_cast<IRawElementProviderFragment*>(this);
        else if(iid==__uuidof(IRawElementProviderFragmentRoot)&&root_)
            *object=static_cast<IRawElementProviderFragmentRoot*>(this);
        else{
        const auto info=Info();
        if(iid==__uuidof(IInvokeProvider)&&info->invoke)*object=static_cast<IInvokeProvider*>(this);
        else if(iid==__uuidof(IValueProvider)&&info->valuePattern)*object=static_cast<IValueProvider*>(this);
        else if(iid==__uuidof(ISelectionProvider)&&info->selection)*object=static_cast<ISelectionProvider*>(this);
        else if(iid==__uuidof(ISelectionItemProvider)&&info->selectionItem)*object=static_cast<ISelectionItemProvider*>(this);
        else if(iid==__uuidof(IToggleProvider)&&info->toggle)*object=static_cast<IToggleProvider*>(this);
        else if(iid==__uuidof(IExpandCollapseProvider)&&info->expandCollapse)*object=static_cast<IExpandCollapseProvider*>(this);
        }
        if(!*object)return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value=--references_;if(!value)delete this;return value; }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if(!result)return E_INVALIDARG;*result=ProviderOptions_ServerSideProvider|ProviderOptions_UseComThreading;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;
        const auto info=Info();
        if(id==UIA_InvokePatternId&&info->invoke)*result=static_cast<IInvokeProvider*>(this);
        else if(id==UIA_ValuePatternId&&info->valuePattern)*result=static_cast<IValueProvider*>(this);
        else if(id==UIA_SelectionPatternId&&info->selection)*result=static_cast<ISelectionProvider*>(this);
        else if(id==UIA_SelectionItemPatternId&&info->selectionItem)*result=static_cast<ISelectionItemProvider*>(this);
        else if(id==UIA_TogglePatternId&&info->toggle)*result=static_cast<IToggleProvider*>(this);
        else if(id==UIA_ExpandCollapsePatternId&&info->expandCollapse)*result=static_cast<IExpandCollapseProvider*>(this);
        if(*result)AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* result) override {
        if(!result)return E_INVALIDARG;VariantInit(result);const auto info=Info();
        if(!info->valid)return UIA_E_ELEMENTNOTAVAILABLE;
        switch(id){
        case UIA_ControlTypePropertyId:return SetInt(result,info->controlType);
        case UIA_NamePropertyId:return SetString(result,info->name);
        case UIA_AutomationIdPropertyId:return SetString(result,info->automationId);
        case UIA_ClassNamePropertyId:return SetString(result,info->className);
        case UIA_FrameworkIdPropertyId:return SetString(result,L"TWebFrame");
        case UIA_HelpTextPropertyId:return SetString(result,info->helpText);
        case UIA_AriaRolePropertyId:return SetString(result,info->ariaRole);
        case UIA_AriaPropertiesPropertyId:return SetString(result,info->ariaProperties);
        case UIA_IsEnabledPropertyId:return SetBool(result,info->enabled);
        case UIA_IsKeyboardFocusablePropertyId:return SetBool(result,info->focusable);
        case UIA_HasKeyboardFocusPropertyId:return SetBool(result,info->focused);
        case UIA_IsOffscreenPropertyId:return SetBool(result,info->offscreen);
        case UIA_IsPasswordPropertyId:return SetBool(result,info->password);
        case UIA_IsControlElementPropertyId:case UIA_IsContentElementPropertyId:return SetBool(result,true);
        case UIA_LiveSettingPropertyId:return SetInt(result,info->liveSetting);
        case UIA_NativeWindowHandlePropertyId:return SetInt(result,root_?static_cast<int>(reinterpret_cast<INT_PTR>(host_->Window())):0);
        default:return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;
        return root_&&host_&&host_->Window()?UiaHostProviderFromHwnd(host_->Window(),result):S_OK;
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        std::shared_ptr<Node> target;bool targetRoot=false;
        if(direction==NavigateDirection_FirstChild||direction==NavigateDirection_LastChild){
            const auto children=host_->Children(root_?std::shared_ptr<Node>{}:node_);
            if(!children.empty())target=direction==NavigateDirection_FirstChild?children.front():children.back();
        }else if(!root_&&direction==NavigateDirection_Parent){
            target=host_->Parent(node_);targetRoot=!target;
        }else if(!root_&&(direction==NavigateDirection_NextSibling||direction==NavigateDirection_PreviousSibling)){
            target=host_->Sibling(node_,direction==NavigateDirection_NextSibling);
        }
        if(target||targetRoot)*result=static_cast<IRawElementProviderFragment*>(New(target,targetRoot));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if(!result)return E_INVALIDARG;*result=SafeArrayCreateVector(VT_I4,0,4);if(!*result)return E_OUTOFMEMORY;
        const auto window=static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(host_?host_->Window():nullptr));
        const auto identity=root_?window:static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(node_.get()));
        int values[4]={UiaAppendRuntimeId,static_cast<int>(window),static_cast<int>(identity),
                       static_cast<int>(identity>>32)};
        for(LONG index=0;index<4;++index)SafeArrayPutElement(*result,&index,&values[index]);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if(!result)return E_INVALIDARG;const auto info=Info();if(!info->valid)return UIA_E_ELEMENTNOTAVAILABLE;
        *result=info->bounds;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        return !root_&&host_?host_->Focus(node_):UIA_E_NOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        auto* provider=New({},true);*result=static_cast<IRawElementProviderFragmentRoot*>(provider);return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,
                                                        IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        auto target=host_->FromPoint(x,y);auto* provider=New(target,!target);
        *result=static_cast<IRawElementProviderFragment*>(provider);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        auto target=host_->Focused();if(target)*result=static_cast<IRawElementProviderFragment*>(New(target,false));return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke() override { return host_?host_->Invoke(node_):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override { return host_?host_->SetValue(node_,value?value:L""):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override {
        if(!result)return E_INVALIDARG;const auto info=Info();*result=SysAllocString(info->value.c_str());return *result?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override { if(!result)return E_INVALIDARG;*result=Info()->readOnly;return S_OK; }

    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto children=host_->Children(node_);std::vector<std::shared_ptr<Node>> selected;
        for(const auto& child:children)if(host_->Info(child).selected)selected.push_back(child);
        *result=SafeArrayCreateVector(VT_UNKNOWN,0,static_cast<ULONG>(selected.size()));if(!*result)return E_OUTOFMEMORY;
        for(LONG index=0;index<static_cast<LONG>(selected.size());++index){
            IUnknown* unknown=static_cast<IRawElementProviderSimple*>(New(selected[index],false));
            SafeArrayPutElement(*result,&index,unknown);unknown->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override { if(!result)return E_INVALIDARG;*result=FALSE;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override { if(!result)return E_INVALIDARG;*result=TRUE;return S_OK; }

    HRESULT STDMETHODCALLTYPE Select() override { return host_?host_->Select(node_):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override { if(!result)return E_INVALIDARG;*result=Info()->selected;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;if(!host_)return UIA_E_ELEMENTNOTAVAILABLE;
        auto parent=host_->Parent(node_);if(parent)*result=static_cast<IRawElementProviderSimple*>(New(parent,false));return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Toggle() override { return host_?host_->Toggle(node_):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* result) override {
        if(!result)return E_INVALIDARG;const auto info=Info();*result=info->mixed?ToggleState_Indeterminate:
            info->checked?ToggleState_On:ToggleState_Off;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Expand() override { return host_?host_->Expand(node_,true):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE Collapse() override { return host_?host_->Expand(node_,false):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* result) override {
        if(!result)return E_INVALIDARG;*result=Info()->expanded?ExpandCollapseState_Expanded:ExpandCollapseState_Collapsed;return S_OK;
    }

private:
    std::shared_ptr<const AccessibilityNodeInfo> Info() const {
        static const auto empty=std::make_shared<const AccessibilityNodeInfo>();
        if(!host_)return empty;
        std::lock_guard<std::mutex> lock(infoMutex_);
        auto revision=host_->Revision();
        if(cachedInfo_&&cachedRevision_==revision)return cachedInfo_;
        AccessibilityNodeInfo value;
        do{
            revision=host_->Revision();
            value=host_->Info(root_?std::shared_ptr<Node>{}:node_);
        }while(revision!=host_->Revision());
        cachedInfo_=std::make_shared<const AccessibilityNodeInfo>(std::move(value));
        cachedRevision_=revision;
        return cachedInfo_;
    }
    AccessibilityProvider* New(const std::shared_ptr<Node>& node,bool root) const {
        return new AccessibilityProvider(host_,node,root);
    }
    static HRESULT SetString(VARIANT* result,const std::wstring& value){result->vt=VT_BSTR;result->bstrVal=SysAllocString(value.c_str());return result->bstrVal?S_OK:E_OUTOFMEMORY;}
    static HRESULT SetBool(VARIANT* result,bool value){result->vt=VT_BOOL;result->boolVal=value?VARIANT_TRUE:VARIANT_FALSE;return S_OK;}
    static HRESULT SetInt(VARIANT* result,int value){result->vt=VT_I4;result->lVal=value;return S_OK;}

    std::atomic<ULONG> references_{1};
    std::shared_ptr<AccessibilityHost> host_;
    std::shared_ptr<Node> node_;
    bool root_ = false;
    mutable std::mutex infoMutex_;
    mutable std::shared_ptr<const AccessibilityNodeInfo> cachedInfo_;
    mutable unsigned long long cachedRevision_ = 0;
};

} // namespace

AccessibilityHost::AccessibilityHost(HWND window):window_(window){uiThread_=GetWindowThreadProcessId(window_,nullptr);}
AccessibilityHost::~AccessibilityHost(){Disconnect();}

void AccessibilityHost::SetCallbacks(InfoCallback info,NodesCallback children,NodeCallback parent,
    SiblingCallback sibling,PointCallback point,FocusedCallback focused,ActionCallback focus,ActionCallback invoke,
    ValueCallback setValue,ActionCallback select,ActionCallback toggle,ExpandCallback expand){
    info_=std::move(info);children_=std::move(children);parent_=std::move(parent);sibling_=std::move(sibling);point_=std::move(point);
    focused_=std::move(focused);focus_=std::move(focus);invoke_=std::move(invoke);setValue_=std::move(setValue);
    select_=std::move(select);toggle_=std::move(toggle);expand_=std::move(expand);
}
void AccessibilityHost::Disconnect(){Invalidate();window_=nullptr;info_={};children_={};parent_={};sibling_={};point_={};focused_={};focus_={};invoke_={};setValue_={};select_={};toggle_={};expand_={};}
LRESULT AccessibilityHost::HandleDispatch(LPARAM parameter){
    auto* action=reinterpret_cast<const std::function<void()>*>(parameter);if(action)(*action)();return 0;
}
void AccessibilityHost::OnUiThread(const std::function<void()>& action){
    const HWND window=window_;if(!window||!IsWindow(window))return;
    if(GetCurrentThreadId()==uiThread_)action();else SendMessageW(window,kAccessibilityDispatchMessage,0,reinterpret_cast<LPARAM>(&action));
}
LRESULT AccessibilityHost::ReturnRawProvider(WPARAM wParam,LPARAM lParam){
    auto* provider=new AccessibilityProvider(shared_from_this(),{},true);
    const LRESULT result=UiaReturnRawElementProvider(window_,wParam,lParam,provider);provider->Release();return result;
}
AccessibilityNodeInfo AccessibilityHost::Info(const NodePtr& node){AccessibilityNodeInfo value;OnUiThread([&]{if(info_)value=info_(node);});return value;}
std::vector<AccessibilityHost::NodePtr> AccessibilityHost::Children(const NodePtr& node){std::vector<NodePtr> value;OnUiThread([&]{if(children_)value=children_(node);});return value;}
AccessibilityHost::NodePtr AccessibilityHost::Parent(const NodePtr& node){NodePtr value;OnUiThread([&]{if(parent_)value=parent_(node);});return value;}
AccessibilityHost::NodePtr AccessibilityHost::Sibling(const NodePtr& node,bool next){NodePtr value;OnUiThread([&]{if(sibling_)value=sibling_(node,next);});return value;}
AccessibilityHost::NodePtr AccessibilityHost::FromPoint(double x,double y){NodePtr value;OnUiThread([&]{if(point_)value=point_(x,y);});return value;}
AccessibilityHost::NodePtr AccessibilityHost::Focused(){NodePtr value;OnUiThread([&]{if(focused_)value=focused_();});return value;}
HRESULT AccessibilityHost::Focus(const NodePtr& node){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(focus_)value=focus_(node);});return value;}
HRESULT AccessibilityHost::Invoke(const NodePtr& node){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(invoke_)value=invoke_(node);});return value;}
HRESULT AccessibilityHost::SetValue(const NodePtr& node,const std::wstring& text){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(setValue_)value=setValue_(node,text);});return value;}
HRESULT AccessibilityHost::Select(const NodePtr& node){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(select_)value=select_(node);});return value;}
HRESULT AccessibilityHost::Toggle(const NodePtr& node){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(toggle_)value=toggle_(node);});return value;}
HRESULT AccessibilityHost::Expand(const NodePtr& node,bool expand){HRESULT value=UIA_E_ELEMENTNOTAVAILABLE;OnUiThread([&]{if(expand_)value=expand_(node,expand);});return value;}

void AccessibilityHost::RaiseFocusChanged(const NodePtr& node){
    if(!window_)return;auto* provider=new AccessibilityProvider(shared_from_this(),node,!node);
    UiaRaiseAutomationEvent(provider,UIA_AutomationFocusChangedEventId);provider->Release();
}
void AccessibilityHost::RaisePropertyChanged(const NodePtr& node,PROPERTYID property,const VARIANT& oldValue,const VARIANT& newValue){
    if(!window_)return;auto* provider=new AccessibilityProvider(shared_from_this(),node,!node);
    UiaRaiseAutomationPropertyChangedEvent(provider,property,oldValue,newValue);provider->Release();
}
void AccessibilityHost::RaiseLiveRegionChanged(const NodePtr& node){
    if(!window_)return;auto* provider=new AccessibilityProvider(shared_from_this(),node,!node);
    UiaRaiseAutomationEvent(provider,UIA_LiveRegionChangedEventId);provider->Release();
}

} // namespace TWebFrame::Internal
