#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

// Owns the platform text-service boundary for a TWebFrame HWND. DOM mutation,
// selection and painting stay in View; this class only translates Win32 IME
// messages into renderer-neutral composition callbacks.
class TextInput {
public:
    struct Client {
        std::function<bool()> canEdit;
        std::function<void()> beginComposition;
        std::function<void(const std::wstring&, const std::vector<unsigned char>&, size_t)>
            updateComposition;
        std::function<void(const std::wstring&)> commitComposition;
        std::function<void()> cancelComposition;
        std::function<RECT()> caretRect;
    };

    explicit TextInput(Client client);

    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                       LRESULT& result);
    void UpdateCandidateWindow(HWND window) const;
    void Cancel(HWND window);
    bool IsComposing() const noexcept { return composing_; }

    // Renderer-independent entry points shared by IMM32 today and usable by
    // other Windows text services without creating a native edit control.
    void StartComposition();
    void UpdateComposition(const std::wstring& text,
                           const std::vector<unsigned char>& attributes, size_t cursor);
    void CommitComposition(const std::wstring& text);
    void CancelComposition();

private:
    static std::wstring CompositionString(HIMC context, DWORD index);
    static std::vector<unsigned char> CompositionAttributes(HIMC context);
    Client client_;
    bool composing_ = false;
};

} // namespace TWebFrame::Internal
