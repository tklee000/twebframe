#include "TextInput.h"

#include <imm.h>

#include <algorithm>
#include <utility>

#pragma comment(lib, "imm32.lib")

namespace TWebFrame::Internal {

TextInput::TextInput(Client client) : client_(std::move(client)) {}

std::wstring TextInput::CompositionString(HIMC context, DWORD index) {
    if (!context) return {};
    const LONG byteCount = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (byteCount <= 0) return {};
    std::wstring result(static_cast<size_t>(byteCount) / sizeof(wchar_t), L'\0');
    ImmGetCompositionStringW(context, index, result.data(), byteCount);
    return result;
}

std::vector<unsigned char> TextInput::CompositionAttributes(HIMC context) {
    if (!context) return {};
    const LONG byteCount = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
    if (byteCount <= 0) return {};
    std::vector<unsigned char> result(static_cast<size_t>(byteCount));
    ImmGetCompositionStringW(context, GCS_COMPATTR, result.data(), byteCount);
    return result;
}

void TextInput::StartComposition() {
    if (composing_ || !client_.canEdit || !client_.canEdit()) return;
    composing_ = true;
    if (client_.beginComposition) client_.beginComposition();
}

void TextInput::UpdateComposition(const std::wstring& text,
                                  const std::vector<unsigned char>& attributes, size_t cursor) {
    StartComposition();
    if (!composing_) return;
    if (client_.updateComposition)
        client_.updateComposition(text, attributes, std::min(cursor, text.size()));
}

void TextInput::CommitComposition(const std::wstring& text) {
    StartComposition();
    if (!composing_) return;
    if (client_.commitComposition) client_.commitComposition(text);
    composing_ = false;
}

void TextInput::CancelComposition() {
    if (!composing_) return;
    if (client_.cancelComposition) client_.cancelComposition();
    composing_ = false;
}

void TextInput::UpdateCandidateWindow(HWND window) const {
    if (!window || !client_.caretRect) return;
    const RECT caret = client_.caretRect();
    HIMC context = ImmGetContext(window);
    if (!context) return;

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {caret.left, caret.bottom};
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = {caret.left, caret.bottom};
    candidate.rcArea = caret;
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window, context);
}

void TextInput::Cancel(HWND window) {
    if (window) {
        HIMC context = ImmGetContext(window);
        if (context) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(window, context);
        }
    }
    CancelComposition();
}

bool TextInput::HandleMessage(HWND window, UINT message, WPARAM, LPARAM lParam,
                              LRESULT& result) {
    if (message == WM_IME_STARTCOMPOSITION) {
        StartComposition();
        UpdateCandidateWindow(window);
        result = 0;
        return composing_;
    }
    if (message == WM_IME_ENDCOMPOSITION) {
        CancelComposition();
        result = 0;
        return true;
    }
    if (message != WM_IME_COMPOSITION) return false;

    HIMC context = ImmGetContext(window);
    if (!context) return false;

    // A result and the first composing syllable of the next segment may be
    // delivered together. Commit the result first, then expose the new live
    // composition without waiting for a completed Hangul character.
    if (lParam & GCS_RESULTSTR) CommitComposition(CompositionString(context, GCS_RESULTSTR));
    if (lParam & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS)) {
        const auto text = CompositionString(context, GCS_COMPSTR);
        const auto attributes = CompositionAttributes(context);
        LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        if (cursor < 0) cursor = static_cast<LONG>(text.size());
        UpdateComposition(text, attributes, static_cast<size_t>(cursor));
    }
    ImmReleaseContext(window, context);
    UpdateCandidateWindow(window);
    result = 0;
    return true;
}

} // namespace TWebFrame::Internal
