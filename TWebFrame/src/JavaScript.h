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
    using SelectionProvider = std::function<bool(const std::shared_ptr<Node>&, size_t&, size_t&)>;
    using SelectionSetter = std::function<void(const std::shared_ptr<Node>&, size_t, size_t)>;
    struct EventInit {
        std::wstring key;
        std::wstring data;
        std::wstring inputType;
        int button = 0;
        int detail = 0;
        bool ctrlKey = false;
        bool shiftKey = false;
        bool altKey = false;
        bool metaKey = false;
        bool isComposing = false;
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
    void SetSelectionProvider(SelectionProvider provider);
    void SetSelectionSetter(SelectionSetter setter);
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
