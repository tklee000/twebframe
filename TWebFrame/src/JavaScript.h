#pragma once

#include "DOM.h"

#include <functional>
#include <memory>
#include <string>

namespace TWebFrame::Internal {

class JavaScriptRuntime {
public:
    enum class MutationKind { Paint, Style, Layout, Tree };
    struct Mutation {
        MutationKind kind = MutationKind::Paint;
        std::vector<std::shared_ptr<Node>> targets;
        bool liveRegionMembershipChanged = false;
    };
    using MessageSink = std::function<void(const std::wstring&)>;
    using MutationSink = std::function<void(const Mutation&)>;
    using FrameScheduler = std::function<void()>;
    using TimerScheduler = std::function<void(unsigned)>;
    using ResourceLoader = std::function<bool(const std::wstring&, std::wstring&)>;
    using DialogSink = std::function<void(const std::wstring&)>;
    using FrameMessageSink = std::function<void(const std::shared_ptr<Node>&, const std::wstring&)>;
    using ParentMessageSink = std::function<void(const std::wstring&)>;
    using FocusSink = std::function<void(const std::shared_ptr<Node>&)>;
    using ActivationSink = std::function<void(const std::shared_ptr<Node>&)>;
    using PointerCaptureSink = std::function<void(bool)>;
    using SelectionProvider = std::function<bool(const std::shared_ptr<Node>&, size_t&, size_t&)>;
    using SelectionSetter = std::function<void(const std::shared_ptr<Node>&, size_t, size_t)>;
    struct DomSelection {
        std::shared_ptr<Node> anchorNode;
        size_t anchorOffset = 0;
        std::shared_ptr<Node> focusNode;
        size_t focusOffset = 0;
    };
    using DomSelectionProvider = std::function<bool(DomSelection&)>;
    using DomSelectionSetter = std::function<void(const DomSelection&)>;
    struct EventInit {
        std::wstring key;
        std::wstring data;
        std::wstring inputType;
        std::shared_ptr<Node> relatedTarget;
        double clientX = 0;
        double clientY = 0;
        double pageX = 0;
        double pageY = 0;
        double screenX = 0;
        double screenY = 0;
        double movementX = 0;
        double movementY = 0;
        int button = 0;
        int buttons = 0;
        int detail = 0;
        bool ctrlKey = false;
        bool shiftKey = false;
        bool altKey = false;
        bool metaKey = false;
        bool isComposing = false;
        bool bubbles = true;
        bool cancelable = true;
    };
    struct NodeGeometry {
        double x=0,y=0,width=0,height=0;
        double clientWidth=0,clientHeight=0,scrollWidth=0,scrollHeight=0;
    };
    using GeometryProvider = std::function<NodeGeometry(const std::shared_ptr<Node>&)>;
    using StylePropertyProvider = std::function<std::wstring(const std::shared_ptr<Node>&,
                                                             const std::wstring&)>;

    explicit JavaScriptRuntime(Document& document);
    ~JavaScriptRuntime();
    JavaScriptRuntime(const JavaScriptRuntime&) = delete;
    JavaScriptRuntime& operator=(const JavaScriptRuntime&) = delete;

    void SetMessageSink(MessageSink sink);
    void SetMutationSink(MutationSink sink);
    void SetFrameScheduler(FrameScheduler scheduler);
    void SetTimerScheduler(TimerScheduler scheduler);
    void SetGeometryProvider(GeometryProvider provider);
    void SetStylePropertyProvider(StylePropertyProvider provider);
    void SetResourceLoader(ResourceLoader loader);
    void SetDialogSink(DialogSink sink);
    void SetFrameMessageSink(FrameMessageSink sink);
    void SetParentMessageSink(ParentMessageSink sink);
    void SetFocusSink(FocusSink sink);
    void SetActivationSink(ActivationSink sink);
    void SetPointerCaptureSink(PointerCaptureSink sink);
    void SetSelectionProvider(SelectionProvider provider);
    void SetSelectionSetter(SelectionSetter setter);
    void SetDomSelectionProvider(DomSelectionProvider provider);
    void SetDomSelectionSetter(DomSelectionSetter setter);
    void SetViewportSize(double width, double height);
    void SetDevicePixelRatio(double ratio);
    void SetLocation(const std::wstring& location);
    void NavigateToFragment(const std::wstring& fragment);
    bool Load(const std::wstring& source, std::wstring* error = nullptr);
    bool Execute(const std::wstring& source, std::wstring* result = nullptr,
                 std::wstring* error = nullptr);
    void DispatchDocumentEvent(const std::wstring& eventName);
    void DispatchWindowEvent(const std::wstring& eventName);
    void RunAnimationFrame();
    void RunTimers();
    bool DispatchNodeEvent(const std::shared_ptr<Node>& node, const std::wstring& eventName,
                           const EventInit& init = {});
    bool DispatchClipboardEvent(const std::shared_ptr<Node>& node,
                                const std::wstring& eventName,
                                const std::wstring& text,
                                const std::vector<Node::FileInfo>& files = {});
    std::shared_ptr<Node> CapturedPointerTarget() const;
    void ClearPointerCapture();
    void DispatchFileDrop(const std::shared_ptr<Node>& node,
                          const std::vector<Node::FileInfo>& files);
    bool DispatchWebMessageAsJson(const std::wstring& json, std::wstring* error = nullptr);
    void DispatchWebMessageAsString(const std::wstring& message);
    bool DispatchWindowMessageAsJson(const std::wstring& json,
                                     const std::shared_ptr<Node>& sourceFrame = {},
                                     std::wstring* error = nullptr);
    void Clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TWebFrame::Internal
