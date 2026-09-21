#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace TWebFrame {

inline constexpr unsigned int VersionMajor = 0;
inline constexpr unsigned int VersionMinor = 4;
inline constexpr wchar_t VersionString[] = L"0.4";

// TWebFrame is a small, embeddable HTML/CSS/JavaScript surface.  The API is
// intentionally small and keeps a compatibility bridge for existing local pages
// while keeping ownership and threading rules explicit.
class View final {
public:
    using MessageHandler = std::function<void(const std::wstring&)>;
    using LoadHandler = std::function<void(bool, const std::wstring&)>;
    // Resolves page-relative text resources such as stylesheets, scripts, and
    // JSON requested by fetch(). Return false when a resource is unavailable.
    using ResourceLoader = std::function<bool(const std::wstring&, std::wstring&)>;

    static std::unique_ptr<View> Create(HWND parent, const RECT& bounds);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    HWND Window() const noexcept;
    void SetBounds(const RECT& bounds);
    void SetVisible(bool visible);
    void SetMessageHandler(MessageHandler handler);
    void SetLoadHandler(LoadHandler handler);
    void SetResourceLoader(ResourceLoader loader);

    bool Navigate(const std::wstring& filePath);
    bool NavigateToString(const std::wstring& html, const std::wstring& basePath = L"");

    // Source is always parsed and compiled by TWebFrame's JavaScript compiler.
    // The returned string is the JavaScript result converted to a string.
    bool ExecuteScript(const std::wstring& source, std::wstring* result = nullptr,
                       std::wstring* error = nullptr);
    bool ExecuteScript(const wchar_t* source, std::wstring* result = nullptr,
                       std::wstring* error = nullptr) {
        return ExecuteScript(std::wstring(source ? source : L""), result, error);
    }

    // Delivers a host message through
    // window.twebframe.addEventListener('message', ...). The compatibility
    // window.chrome.webview bridge receives the same message. JSON messages
    // expose the decoded JavaScript value as event.data.
    bool PostWebMessageAsJson(const std::wstring& json, std::wstring* error = nullptr);
    void PostWebMessageAsString(const std::wstring& message);

    // Test/diagnostic helpers. Layout JSON contains stable integer pixel bounds.
    std::wstring DumpLayoutJson() const;
    // Accessibility JSON exposes the same stable automation ids used by the
    // UI Automation provider. Prefer an explicit data-automation-id or id;
    // otherwise TWebFrame emits a deterministic DOM path.
    std::wstring DumpAccessibilityJson() const;
    std::wstring LastError() const;

private:
    struct Impl;
    explicit View(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace TWebFrame
