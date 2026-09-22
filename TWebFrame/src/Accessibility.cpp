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
        : host_(host), node_(node), root_(root),
          registration_(std::make_shared<AccessibilityProviderRegistration>()) {
        // UI Automation may retain a provider after its HWND and document have
        // gone away. The provider therefore observes engine objects weakly.
        // The host tracks a weak registration so it can disconnect COM-side
        // references without keeping every provider alive during the view.
        registration_->provider=static_cast<IRawElementProviderSimple*>(this);
        registration_->node=node;
        registration_->root=root;
    }

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
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG value=0;
        {
            // Disconnect locks the same registration before taking a temporary
            // COM reference, so it can never resurrect a provider whose final
            // Release has already started.
            std::lock_guard<std::mutex> lock(registration_->mutex);
            value=--references_;if(!value)registration_->provider=nullptr;
        }
        if(!value)delete this;return value;
    }

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
        case UIA_NativeWindowHandlePropertyId:{const auto host=host_.lock();return SetInt(result,root_&&host?static_cast<int>(reinterpret_cast<INT_PTR>(host->Window())):0);}
        default:return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;
        const auto host=host_.lock();
        return root_&&host&&host->Window()?UiaHostProviderFromHwnd(host->Window(),result):S_OK;
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();
        const auto node=node_.lock();if(!host||(!root_&&!node))return UIA_E_ELEMENTNOTAVAILABLE;
        std::shared_ptr<Node> target;bool targetRoot=false;
        if(direction==NavigateDirection_FirstChild||direction==NavigateDirection_LastChild){
            const auto children=host->Children(root_?std::shared_ptr<Node>{}:node);
            if(!children.empty())target=direction==NavigateDirection_FirstChild?children.front():children.back();
        }else if(!root_&&direction==NavigateDirection_Parent){
            target=host->Parent(node);targetRoot=!target;
        }else if(!root_&&(direction==NavigateDirection_NextSibling||direction==NavigateDirection_PreviousSibling)){
            target=host->Sibling(node,direction==NavigateDirection_NextSibling);
        }
        if(target||targetRoot)*result=static_cast<IRawElementProviderFragment*>(New(target,targetRoot));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();const auto node=node_.lock();
        if(!host||(!root_&&!node))return UIA_E_ELEMENTNOTAVAILABLE;
        // UIA derives a window-hosted fragment root's runtime ID from its HWND.
        // Returning UiaAppendRuntimeId here prevents UiaDisconnectProvider from
        // resolving the root and leaves the provider cached after destruction.
        if(root_)return S_OK;
        *result=SafeArrayCreateVector(VT_I4,0,4);if(!*result)return E_OUTOFMEMORY;
        const auto window=static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(host->Window()));
        const auto identity=root_?window:static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(node.get()));
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
        const auto host=host_.lock();const auto node=node_.lock();
        return !root_&&host&&node?host->Focus(node):UIA_E_NOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();
        if(!host)return UIA_E_ELEMENTNOTAVAILABLE;
        // A disconnect resolves a child provider through its fragment root
        // after the host has stopped publishing new providers. Constructing
        // this transient root directly keeps that resolution available without
        // reopening the WM_GETOBJECT path during teardown.
        auto* provider=new AccessibilityProvider(host,{},true);
        *result=static_cast<IRawElementProviderFragmentRoot*>(provider);return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,
                                                        IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();if(!host)return UIA_E_ELEMENTNOTAVAILABLE;
        auto target=host->FromPoint(x,y);auto* provider=New(target,!target);
        *result=static_cast<IRawElementProviderFragment*>(provider);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();if(!host)return UIA_E_ELEMENTNOTAVAILABLE;
        auto target=host->Focused();if(target)*result=static_cast<IRawElementProviderFragment*>(New(target,false));return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke() override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->Invoke(node):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->SetValue(node,value?value:L""):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override {
        if(!result)return E_INVALIDARG;const auto info=Info();*result=SysAllocString(info->value.c_str());return *result?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override { if(!result)return E_INVALIDARG;*result=Info()->readOnly;return S_OK; }

    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();const auto node=node_.lock();
        if(!host||!node)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto children=host->Children(node);std::vector<std::shared_ptr<Node>> selected;
        for(const auto& child:children)if(host->Info(child).selected)selected.push_back(child);
        *result=SafeArrayCreateVector(VT_UNKNOWN,0,static_cast<ULONG>(selected.size()));if(!*result)return E_OUTOFMEMORY;
        for(LONG index=0;index<static_cast<LONG>(selected.size());++index){
            IUnknown* unknown=static_cast<IRawElementProviderSimple*>(New(selected[index],false));
            if(!unknown){SafeArrayDestroy(*result);*result=nullptr;return UIA_E_ELEMENTNOTAVAILABLE;}
            SafeArrayPutElement(*result,&index,unknown);unknown->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override { if(!result)return E_INVALIDARG;*result=FALSE;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override { if(!result)return E_INVALIDARG;*result=TRUE;return S_OK; }

    HRESULT STDMETHODCALLTYPE Select() override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->Select(node):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override { if(!result)return E_INVALIDARG;*result=Info()->selected;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override {
        if(!result)return E_INVALIDARG;*result=nullptr;const auto host=host_.lock();const auto node=node_.lock();
        if(!host||!node)return UIA_E_ELEMENTNOTAVAILABLE;
        auto parent=host->Parent(node);if(parent)*result=static_cast<IRawElementProviderSimple*>(New(parent,false));return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Toggle() override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->Toggle(node):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* result) override {
        if(!result)return E_INVALIDARG;const auto info=Info();*result=info->mixed?ToggleState_Indeterminate:
            info->checked?ToggleState_On:ToggleState_Off;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Expand() override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->Expand(node,true):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE Collapse() override { const auto host=host_.lock();const auto node=node_.lock();return host&&node?host->Expand(node,false):UIA_E_ELEMENTNOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* result) override {
        if(!result)return E_INVALIDARG;*result=Info()->expanded?ExpandCollapseState_Expanded:ExpandCollapseState_Collapsed;return S_OK;
    }

    const std::shared_ptr<AccessibilityProviderRegistration>& Registration() const noexcept {
        return registration_;
    }

private:
    std::shared_ptr<const AccessibilityNodeInfo> Info() const {
        static const auto empty=std::make_shared<const AccessibilityNodeInfo>();
        const auto host=host_.lock();const auto node=node_.lock();
        if(!host||(!root_&&!node))return empty;
        std::lock_guard<std::mutex> lock(infoMutex_);
        auto revision=host->Revision();
        if(cachedInfo_&&cachedRevision_==revision)return cachedInfo_;
        AccessibilityNodeInfo value;
        do{
            revision=host->Revision();
            value=host->Info(root_?std::shared_ptr<Node>{}:node);
        }while(revision!=host->Revision());
        cachedInfo_=std::make_shared<const AccessibilityNodeInfo>(std::move(value));
        cachedRevision_=revision;
        return cachedInfo_;
    }
    AccessibilityProvider* New(const std::shared_ptr<Node>& node,bool root) const {
        const auto host=host_.lock();
        return host?static_cast<AccessibilityProvider*>(host->ProviderFor(node,root)):nullptr;
    }
    static HRESULT SetString(VARIANT* result,const std::wstring& value){result->vt=VT_BSTR;result->bstrVal=SysAllocString(value.c_str());return result->bstrVal?S_OK:E_OUTOFMEMORY;}
    static HRESULT SetBool(VARIANT* result,bool value){result->vt=VT_BOOL;result->boolVal=value?VARIANT_TRUE:VARIANT_FALSE;return S_OK;}
    static HRESULT SetInt(VARIANT* result,int value){result->vt=VT_I4;result->lVal=value;return S_OK;}

    std::atomic<ULONG> references_{1};
    std::weak_ptr<AccessibilityHost> host_;
    std::weak_ptr<Node> node_;
    bool root_ = false;
    mutable std::mutex infoMutex_;
    mutable std::shared_ptr<const AccessibilityNodeInfo> cachedInfo_;
    mutable unsigned long long cachedRevision_ = 0;
    std::shared_ptr<AccessibilityProviderRegistration> registration_;
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
IRawElementProviderSimple* AccessibilityHost::ProviderFor(const NodePtr& node,bool root){
    std::vector<IRawElementProviderSimple*> staleProviders;
    IRawElementProviderSimple* result=nullptr;
    {
        std::lock_guard<std::mutex> lock(providersMutex_);
        if(disconnected_)return nullptr;

        // Keep one provider identity per live DOM node (and one for the root).
        // Repeated UIA navigation must not grow the provider list for the life
        // of a view. Providers for collected DOM nodes are disconnected on the
        // next lookup and removed from both tracking containers.
        for(auto it=providers_.begin();it!=providers_.end();){
            const auto registration=it->lock();
            if(!registration){it=providers_.erase(it);continue;}
            if(!registration->root&&registration->node.expired()){
                std::lock_guard<std::mutex> providerLock(registration->mutex);
                if(registration->provider){
                    registration->provider->AddRef();
                    staleProviders.push_back(registration->provider);
                }
                it=providers_.erase(it);continue;
            }
            ++it;
        }
        for(auto it=nodeProviders_.begin();it!=nodeProviders_.end();){
            const auto registration=it->second.lock();
            if(!registration||registration->node.expired())it=nodeProviders_.erase(it);
            else ++it;
        }

        auto registration=root?rootProvider_.lock():
            (node?([&]{const auto found=nodeProviders_.find(node.get());
                       return found==nodeProviders_.end()?std::shared_ptr<AccessibilityProviderRegistration>{}:
                           found->second.lock();})():std::shared_ptr<AccessibilityProviderRegistration>{});
        if(registration&&registration->root==root&&
           (root||registration->node.lock()==node)){
            std::lock_guard<std::mutex> providerLock(registration->mutex);
            if(registration->provider){registration->provider->AddRef();result=registration->provider;}
        }
        if(!result){
            auto* provider=new AccessibilityProvider(shared_from_this(),node,root);
            registration=provider->Registration();
            providers_.push_back(registration);
            if(root)rootProvider_=registration;
            else if(node)nodeProviders_[node.get()]=registration;
            result=provider;
        }
    }
    for(auto* provider:staleProviders){UiaDisconnectProvider(provider);provider->Release();}
    return result;
}
void AccessibilityHost::Disconnect(){
    std::vector<std::weak_ptr<AccessibilityProviderRegistration>> registrations;
    {
        std::lock_guard<std::mutex> lock(providersMutex_);
        if(!disconnected_){
            disconnected_=true;registrations.swap(providers_);
            rootProvider_.reset();nodeProviders_.clear();
        }
    }
    std::vector<IRawElementProviderSimple*> providers;
    providers.reserve(registrations.size());
    for(const auto& weak:registrations)if(const auto registration=weak.lock()){
        std::lock_guard<std::mutex> lock(registration->mutex);
        if(registration->provider){registration->provider->AddRef();providers.push_back(registration->provider);}
    }
    // UIAutomationCore can cache providers beyond WM_DESTROY. Explicitly
    // disconnect every provider still retained outside TWebFrame. This is
    // scoped to the destroyed view and does not disturb other controls.
    for(auto* provider:providers){
        const HRESULT result=UiaDisconnectProvider(provider);
        if(FAILED(result)){
            wchar_t message[96]{};
            swprintf_s(message,L"TWebFrame: UiaDisconnectProvider failed (0x%08X, send=0x%X)\r\n",
                       static_cast<unsigned>(result),InSendMessageEx(nullptr));
            OutputDebugStringW(message);
        }
    }
    Invalidate();window_=nullptr;info_={};children_={};parent_={};sibling_={};point_={};focused_={};focus_={};invoke_={};setValue_={};select_={};toggle_={};expand_={};
    for(auto* provider:providers)provider->Release();
}
LRESULT AccessibilityHost::HandleDispatch(LPARAM parameter){
    auto* action=reinterpret_cast<const std::function<void()>*>(parameter);if(action)(*action)();return 0;
}
void AccessibilityHost::OnUiThread(const std::function<void()>& action){
    const HWND window=window_;if(!window||!IsWindow(window))return;
    if(GetCurrentThreadId()==uiThread_)action();else SendMessageW(window,kAccessibilityDispatchMessage,0,reinterpret_cast<LPARAM>(&action));
}
LRESULT AccessibilityHost::ReturnRawProvider(WPARAM wParam,LPARAM lParam){
    {
        std::lock_guard<std::mutex> lock(providersMutex_);
        if(disconnected_||!window_)return 0;
    }
    auto* provider=ProviderFor({},true);
    if(!provider)return 0;
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
    if(!window_)return;auto* provider=ProviderFor(node,!node);if(!provider)return;
    UiaRaiseAutomationEvent(provider,UIA_AutomationFocusChangedEventId);provider->Release();
}
void AccessibilityHost::RaisePropertyChanged(const NodePtr& node,PROPERTYID property,const VARIANT& oldValue,const VARIANT& newValue){
    if(!window_)return;auto* provider=ProviderFor(node,!node);if(!provider)return;
    UiaRaiseAutomationPropertyChangedEvent(provider,property,oldValue,newValue);provider->Release();
}
void AccessibilityHost::RaiseLiveRegionChanged(const NodePtr& node){
    if(!window_)return;auto* provider=ProviderFor(node,!node);if(!provider)return;
    UiaRaiseAutomationEvent(provider,UIA_LiveRegionChangedEventId);provider->Release();
}

} // namespace TWebFrame::Internal
