#include <TWebFrame/TWebFrame.h>

#include <windows.h>
#include <ole2.h>

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;
volatile LONG firstChanceCppExceptions = 0;

LONG CALLBACK CountFirstChanceCppExceptions(EXCEPTION_POINTERS* exception) {
    if (exception && exception->ExceptionRecord &&
        exception->ExceptionRecord->ExceptionCode == 0xe06d7363UL) {
        InterlockedIncrement(&firstChanceCppExceptions);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

bool ScriptPoint(TWebFrame::View& view, const wchar_t* id, float& x, float& y) {
    std::wstring result, error;
    const std::wstring script =
        L"const r=document.getElementById('" + std::wstring(id) +
        L"').getBoundingClientRect();return (r.x+r.width/2)+','+(r.y+r.height/2);";
    if (!view.ExecuteScript(script, &result, &error)) {
        std::wcerr << error << L'\n';
        return false;
    }
    const auto comma = result.find(L',');
    if (comma == std::wstring::npos) return false;
    try {
        x = std::stof(result.substr(0, comma));
        y = std::stof(result.substr(comma + 1));
        return true;
    } catch (...) {
        return false;
    }
}

void Click(TWebFrame::View& view, const wchar_t* id, float scale) {
    float x = 0, y = 0;
    Check(ScriptPoint(view, id, x, y), L"a generic DOM target exposes click geometry");
    const LPARAM point = MAKELPARAM(static_cast<int>(std::lround(x * scale)),
                                    static_cast<int>(std::lround(y * scale)));
    SendMessageW(view.Window(), WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(view.Window(), WM_LBUTTONUP, 0, point);
}

UINT CheckFormatBlockAtDpi(DPI_AWARENESS_CONTEXT context) {
    const auto previous = SetThreadDpiAwarenessContext(context);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"",
                                WS_POPUP | WS_VISIBLE,
                                workArea.left + 80, workArea.top + 80, 600, 360,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(host != nullptr, L"the editing-command regression host is created");
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    if (host) {
        dpi = GetDpiForWindow(host);
        const float scale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
        RECT bounds{0, 0, static_cast<LONG>(std::lround(360.0f * scale)),
                          static_cast<LONG>(std::lround(220.0f * scale))};
        auto view = TWebFrame::View::Create(host, bounds);
        Check(view != nullptr, L"the editing-command regression view is created");
        if (view) {
            InterlockedExchange(&firstChanceCppExceptions, 0);
            std::wstring message;
            view->SetMessageHandler([&](const std::wstring& value) { message = value; });
            const wchar_t* html = LR"HTML(
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    #editor { display: block; width: 320px; height: 80px; padding: 10px; font: 16px/24px "Segoe UI"; }
                    #editor pre { padding: 10px 12px; background: #162033; }
                    #picker { position: absolute; left: 8px; top: 96px; width: 150px; }
                    button { display: block; width: 140px; height: 32px; }
                    #menu { position: absolute; left: 0; top: 32px; width: 140px; }
                    #menu[hidden] { display: none; }
                </style>
                <article id="editor" contenteditable="true"><p id="line">alpha</p></article>
                <div id="picker">
                  <button id="open" type="button">Code</button>
                  <div id="menu" hidden>
                    <button id="code" type="button">Code block</button>
                    <button id="table" type="button">Insert table</button>
                  </div>
                </div>
                <script>
                  let remembered = null;
                  function rememberSelection() {
                    const selection = window.getSelection();
                    if (selection && selection.rangeCount) remembered = selection.getRangeAt(0).cloneRange();
                  }
                  document.querySelectorAll('#picker button').forEach(button =>
                    button.addEventListener('pointerdown', rememberSelection));
                  document.getElementById('open').addEventListener('click', () => {
                    document.getElementById('menu').hidden = false;
                  });
                  document.getElementById('code').addEventListener('click', () => {
                    const editor = document.getElementById('editor');
                    editor.focus();
                    const selection = window.getSelection();
                    if (remembered) {
                      selection.removeAllRanges();
                      selection.addRange(remembered);
                    }
                    const range = selection.getRangeAt(0);
                    const prefix = document.createRange();
                    prefix.setStart(editor, 0);
                    prefix.setEnd(range.startContainer, range.startOffset);
                    const snapshot = editor.cloneNode(true).innerHTML;
                    const before = editor.innerHTML;
                    const formatted = document.execCommand('formatBlock', false, 'pre');
                    editor.normalize();
                    window.chrome.webview.postMessage(
                      String(formatted) + '|' + snapshot + '|' + before + '|' + editor.innerHTML + '|' +
                      editor.firstElementChild.tagName + '|' + document.activeElement.id + '|' +
                      prefix.toString().length + '|' + selection.rangeCount);
                  });
                  document.getElementById('menu').addEventListener('click', (event) => {
                    if (!event.target.closest('#table')) return;
                    const editor = document.getElementById('editor');
                    editor.focus();
                    const selection = window.getSelection();
                    if (remembered) {
                      selection.removeAllRanges();
                      selection.addRange(remembered);
                    }
                    const inserted = document.execCommand('insertHTML', false,
                      '<table><thead><tr><th>Column 1</th><th>Column 2</th></tr></thead>' +
                      '<tbody><tr><td><br></td><td><br></td></tr>' +
                      '<tr><td><br></td><td><br></td></tr></tbody></table><p><br></p>');
                    editor.normalize();
                    const created = editor.querySelector('table');
                    window.chrome.webview.postMessage(
                      'table|' + String(inserted) + '|' + String(Boolean(created)) + '|' +
                      (created?.parentElement === editor) + '|' +
                      (created?.querySelectorAll('th').length || 0) + '|' +
                      (created?.querySelectorAll('tbody tr').length || 0) + '|' +
                      document.activeElement.id + '|' + editor.innerHTML);
                  });
                </script>
            )HTML";
            Check(view->NavigateToString(html), L"the generic editable toolbar fixture loads");
            std::wstring dispatchResult, dispatchError;
            Check(view->ExecuteScript(
                      L"return '  TABLE  '.trim().toLowerCase();",
                      &dispatchResult, &dispatchError) && dispatchResult == L"table",
                  L"common string methods dispatch normally");
            Click(*view, L"line", scale);
            Click(*view, L"open", scale);
            Click(*view, L"code", scale);
            Check(message.find(L"true|") == 0,
                  L"document.execCommand reports a successful common formatBlock operation");
            if (message.find(L"|<pre id=\"line\">alpha</pre>|PRE|editor|") == std::wstring::npos)
                std::wcerr << L"formatBlock state: " << message << L'\n';
            Check(message.find(L"|<pre id=\"line\">alpha</pre>|PRE|editor|") != std::wstring::npos,
                  L"the saved DOM range is restored and its block becomes PRE");
            Check(message.rfind(L"|1") == message.size() - 2,
                  L"the restored selection remains available after formatting");

            SendMessageW(view->Window(), WM_CHAR, static_cast<WPARAM>(L'X'), 1);
            std::wstring result, error;
            Check(view->ExecuteScript(
                      L"return document.getElementById('editor').firstElementChild.tagName+'|' +"
                      L"document.activeElement.id+'|'+document.getElementById('editor').textContent;",
                      &result, &error) && result.find(L"PRE|editor|") == 0 &&
                      result.find(L'X') != std::wstring::npos,
                  L"typing enters the newly formatted code block without another editor click");

            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');editor.innerHTML='';"
                      L"document.getElementById('menu').hidden=true;return editor.innerHTML;",
                      &result, &error),
                  L"the generic editable fixture resets to an empty document");
            Click(*view, L"editor", scale);
            Click(*view, L"open", scale);
            Click(*view, L"code", scale);
            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');"
                      L"const block=editor.firstElementChild;"
                      L"const rect=block?block.getBoundingClientRect():null;"
                      L"return (block?block.tagName:'')+'|' + (rect?rect.height:0)+'|' +"
                      L"document.activeElement.id;",
                      &result, &error) && result.find(L"PRE|") == 0 &&
                      result.rfind(L"|editor") == result.size() - 7,
                  L"an empty code-block command creates PRE and keeps editor focus");
            const auto firstSeparator = result.find(L'|');
            const auto secondSeparator = result.find(L'|', firstSeparator + 1);
            float emptyBlockHeight = 0;
            if (firstSeparator != std::wstring::npos && secondSeparator != std::wstring::npos) {
                try {
                    emptyBlockHeight = std::stof(result.substr(
                        firstSeparator + 1, secondSeparator - firstSeparator - 1));
                } catch (...) {}
            }
            Check(emptyBlockHeight >= 19.5f,
                  L"an empty padded code block has a visible box before typing");

            SendMessageW(view->Window(), WM_CHAR, static_cast<WPARAM>(L'Y'), 1);
            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');"
                      L"const block=editor.firstElementChild;"
                      L"return block.tagName+'|'+editor.textContent+'|' +"
                      L"block.getBoundingClientRect().height;",
                      &result, &error) && result.find(L"PRE|") == 0 &&
                      result.find(L'Y') != std::wstring::npos,
                  L"typing still enters the already visible empty code block");
            float populatedBlockHeight = 0;
            const auto heightSeparator = result.rfind(L'|');
            if (heightSeparator != std::wstring::npos) {
                try {
                    populatedBlockHeight = std::stof(result.substr(heightSeparator + 1));
                } catch (...) {}
            }
            Check(std::abs(emptyBlockHeight - populatedBlockHeight) < 0.5f,
                  L"an empty editable block reserves the same line height as typed content");

            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');"
                      L"editor.innerHTML='<p id=\"line\">alpha</p>';"
                      L"document.getElementById('menu').hidden=false;return editor.innerHTML;",
                      &result, &error), error.c_str());
            Click(*view, L"line", scale);
            Click(*view, L"table", scale);
            if(message.find(L"table|true|true|true|2|2|editor|")!=0)
                std::wcerr<<L"insertHTML state: "<<message<<L'\n';
            Check(message.find(L"table|true|true|true|2|2|editor|")==0,
                  L"insertHTML creates a selected table as a direct editable block");
            Check(message.find(L"<table><thead><tr><th>Column 1</th><th>Column 2</th>")!=
                      std::wstring::npos,
                  L"insertHTML preserves the table fragment structure and cell content");

            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');editor.innerHTML='';"
                      L"document.getElementById('menu').hidden=false;return editor.innerHTML;",
                      &result, &error), error.c_str());
            Click(*view, L"editor", scale);
            Click(*view, L"table", scale);
            Check(message.find(L"table|true|true|true|2|2|editor|<table>")==0,
                  L"insertHTML creates a table at the caret of an initially empty editor");
            SendMessageW(view->Window(), WM_CHAR, static_cast<WPARAM>(L'Z'), 1);
            Check(view->ExecuteScript(
                      L"const editor=document.getElementById('editor');"
                      L"return editor.querySelectorAll('table').length+'|' +"
                      L"editor.lastElementChild.tagName+'|'+editor.lastElementChild.textContent;",
                      &result, &error) && result==L"1|P|Z",
                  L"typing continues in the trailing paragraph after an inserted table");
            InvalidateRect(view->Window(), nullptr, FALSE);
            UpdateWindow(view->Window());
            Check(InterlockedCompareExchange(&firstChanceCppExceptions, 0, 0) == 0,
                  L"successful editing commands do not use C++ exceptions for normal dispatch");
        }
        DestroyWindow(host);
    }
    SetThreadDpiAwarenessContext(previous);
    return dpi;
}

} // namespace

int wmain() {
    const auto exceptionHandler =
        AddVectoredExceptionHandler(1, CountFirstChanceCppExceptions);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const UINT dpi100 = CheckFormatBlockAtDpi(DPI_AWARENESS_CONTEXT_UNAWARE);
    const UINT monitorDpi = CheckFormatBlockAtDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(dpi100 == USER_DEFAULT_SCREEN_DPI,
          L"the DPI-unaware editing path uses 100 percent coordinates");
    Check(monitorDpi >= USER_DEFAULT_SCREEN_DPI,
          L"the per-monitor editing path uses the monitor DPI coordinates");
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (exceptionHandler) RemoveVectoredExceptionHandler(exceptionHandler);
    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Editing command regression tests passed at 100% and monitor DPI ("
               << monitorDpi << L" DPI)\n";
    return 0;
}
