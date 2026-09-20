#pragma once

#include "DOM.h"

#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

constexpr UINT kAccessibilityDispatchMessage = WM_APP + 0x58;

struct AccessibilityNodeInfo {
    bool valid = false;
    bool root = false;
    bool enabled = true;
    bool focusable = false;
    bool focused = false;
    bool offscreen = false;
    bool readOnly = false;
    bool password = false;
    bool selected = false;
    bool checked = false;
    bool mixed = false;
    bool expanded = false;
    int controlType = UIA_GroupControlTypeId;
    int liveSetting = Off;
    UiaRect bounds{};
    std::wstring name;
    std::wstring automationId;
    std::wstring className;
    std::wstring value;
    std::wstring helpText;
    std::wstring ariaRole;
    std::wstring ariaProperties;
    bool invoke = false;
    bool valuePattern = false;
    bool selection = false;
    bool selectionItem = false;
    bool toggle = false;
    bool expandCollapse = false;
};

class AccessibilityHost final : public std::enable_shared_from_this<AccessibilityHost> {
public:
    using NodePtr = std::shared_ptr<Node>;
    using InfoCallback = std::function<AccessibilityNodeInfo(const NodePtr&)>;
    using NodesCallback = std::function<std::vector<NodePtr>(const NodePtr&)>;
    using NodeCallback = std::function<NodePtr(const NodePtr&)>;
    using SiblingCallback = std::function<NodePtr(const NodePtr&, bool)>;
    using PointCallback = std::function<NodePtr(double, double)>;
    using FocusedCallback = std::function<NodePtr()>;
    using ActionCallback = std::function<HRESULT(const NodePtr&)>;
    using ValueCallback = std::function<HRESULT(const NodePtr&, const std::wstring&)>;
    using ExpandCallback = std::function<HRESULT(const NodePtr&, bool)>;

    explicit AccessibilityHost(HWND window);
    ~AccessibilityHost();

    void SetCallbacks(InfoCallback info, NodesCallback children, NodeCallback parent,
                      SiblingCallback sibling,
                      PointCallback point, FocusedCallback focused, ActionCallback focus,
                      ActionCallback invoke, ValueCallback setValue, ActionCallback select,
                      ActionCallback toggle, ExpandCallback expand);
    void Disconnect();
    void Invalidate() noexcept { revision_.fetch_add(1,std::memory_order_relaxed); }
    unsigned long long Revision() const noexcept {
        return revision_.load(std::memory_order_relaxed);
    }
    LRESULT HandleDispatch(LPARAM parameter);
    LRESULT ReturnRawProvider(WPARAM wParam, LPARAM lParam);

    AccessibilityNodeInfo Info(const NodePtr& node);
    std::vector<NodePtr> Children(const NodePtr& node);
    NodePtr Parent(const NodePtr& node);
    NodePtr Sibling(const NodePtr& node, bool next);
    NodePtr FromPoint(double x, double y);
    NodePtr Focused();
    HRESULT Focus(const NodePtr& node);
    HRESULT Invoke(const NodePtr& node);
    HRESULT SetValue(const NodePtr& node, const std::wstring& value);
    HRESULT Select(const NodePtr& node);
    HRESULT Toggle(const NodePtr& node);
    HRESULT Expand(const NodePtr& node, bool expand);
    HWND Window() const noexcept { return window_; }

    void RaiseFocusChanged(const NodePtr& node);
    void RaisePropertyChanged(const NodePtr& node, PROPERTYID property,
                              const VARIANT& oldValue, const VARIANT& newValue);
    void RaiseLiveRegionChanged(const NodePtr& node);

private:
    void OnUiThread(const std::function<void()>& action);

    HWND window_ = nullptr;
    DWORD uiThread_ = 0;
    InfoCallback info_;
    NodesCallback children_;
    NodeCallback parent_;
    SiblingCallback sibling_;
    PointCallback point_;
    FocusedCallback focused_;
    ActionCallback focus_;
    ActionCallback invoke_;
    ValueCallback setValue_;
    ActionCallback select_;
    ActionCallback toggle_;
    ExpandCallback expand_;
    std::atomic<unsigned long long> revision_{1};
};

} // namespace TWebFrame::Internal
