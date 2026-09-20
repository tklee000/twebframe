# TWebFrame

**Current version: 0.2 (experimental)**

See the [version history](docs/CHANGELOG.md) for cumulative release notes.

TWebFrame is a small, Windows-native HTML UI frame for C and C++ applications. It provides an embeddable `HWND`, a C++ host API, and a shared HTML, DOM, CSS, layout, and JavaScript runtime implemented by this project.

TWebFrame is **not a general-purpose web browser** and version 0.2 does **not** claim complete HTML, CSS, DOM, Web API, or ECMAScript conformance. Its current purpose is to render trusted, application-owned HTML user interfaces and connect them to a native C/C++ host without embedding Chromium, WebView2, CEF, or an external JavaScript engine.

> TWebFrame 0.2 should be treated as an application UI runtime, not as a browser security boundary. Do not use it to display arbitrary or untrusted Internet content.

## Project goals

- Provide a lightweight HTML UI surface that can be owned and positioned like any other child `HWND`.
- Keep C++ ownership, resource loading, messaging, window lifetime, and threading explicit.
- Implement reusable JavaScript, DOM, CSS, and layout behavior instead of page-specific function names, element IDs, or rendering exceptions.
- Support deterministic application UI rendering, input, diagnostics, and accessibility on Windows.
- Grow the common engines according to real native-application UI requirements rather than attempt immediate browser-wide compatibility.

## Host integration

The public API is `TWebFrame::View` in `<TWebFrame/TWebFrame.h>`.

```cpp
#include <TWebFrame/TWebFrame.h>

RECT bounds{};
GetClientRect(parentWindow, &bounds);

auto view = TWebFrame::View::Create(parentWindow, bounds);

view->SetMessageHandler([](const std::wstring& message) {
    // Receives window.chrome.webview.postMessage(...) from JavaScript.
});

view->SetLoadHandler([](bool success, const std::wstring& error) {
    // Called after HTML, CSS, and scripts have been processed.
});

view->SetResourceLoader([](const std::wstring& path, std::wstring& text) {
    // Resolve application-owned stylesheets, scripts, iframe documents,
    // and JSON requested through fetch().
    return false;
});

view->NavigateToString(LR"(
    <button id="run">Run</button>
    <script>
        document.getElementById('run').addEventListener('click', () => {
            window.chrome.webview.postMessage('run');
        });
    </script>
)");

std::wstring result;
std::wstring error;
view->ExecuteScript(L"return document.getElementById('run').textContent;",
                    &result, &error);
```

The host can send messages in the other direction with `PostWebMessageAsJson()` or `PostWebMessageAsString()`. The `window.chrome.webview` name is retained as a compatibility bridge for existing local application pages; TWebFrame does not use WebView2.

## Version 0.2 architecture

| Component | Responsibility |
| --- | --- |
| `DOM.cpp` | HTML tokenization, tree construction, entities, attributes, and fragment parsing |
| `CSS.cpp` | Stylesheet parsing, selector matching, specificity, cascade, inheritance, custom properties, and media-query state |
| `JavaScript.cpp` | Lexer, recursive-descent compiler, bytecode VM, promises, microtasks, timers, DOM bindings, and host bridges |
| `Layout.cpp` | Block, inline, flex, grid, and table layout; painting; hit testing; scrolling; transitions |
| `View.cpp` | Child window, Direct2D/DirectWrite rendering, input, events, iframe views, and native host integration |
| `TextInput.cpp` | Win32 IMM32 composition and candidate-window integration |
| `Accessibility.cpp` | UI Automation fragment tree, properties, actions, and control patterns |

HTML parsing, script execution, DOM mutation, style calculation, layout, paint, hit testing, and event dispatch use the same common path. The engine does not recognize application-specific function names or screen IDs.

## Currently implemented

The following list describes the tested version 0.2 implementation. It is a supported subset, not a standards-compliance statement.

### HTML and DOM

- Document, element, and text nodes; common document, sectioning, phrasing, table, form, and control elements.
- Common attributes, boolean attributes, inline styles, `id`, `class`, `data-*`, and ARIA attributes.
- HTML entities, fragment parsing, `innerHTML`, `innerText`, and `textContent`.
- `getElementById`, `getElementsByName`, `querySelector`, `querySelectorAll`, and `createElement`.
- Tree operations including `appendChild`, `append`, `insertBefore`, `replaceChildren`, `replaceWith`, and `remove`.
- Element APIs including `matches`, `closest`, `contains`, `children`, sibling accessors, attributes, `classList`, `style`, and `dataset`.
- Control state for buttons, checkboxes, radio buttons, text/password/number/file inputs, textareas, selects, options, datalists, dialogs, forms, and `contenteditable` regions.
- Basic dialog `show()`, `showModal()`, and `close()` behavior, plus form `requestSubmit()`.
- Event listeners, bubbling, cancellation, propagation control, focus events, pointer/mouse events, keyboard events, input/change events, scrolling, file drop, document load, and window messages.
- Basic iframe documents loaded by the host resource loader, with separate child views and parent/frame `postMessage` communication.
- DOM geometry and diagnostics through `getBoundingClientRect()`, `DumpLayoutJson()`, and `DumpAccessibilityJson()`.

### CSS, layout, and rendering

- Tag, ID, class, compound, descendant, child, adjacent-sibling, general-sibling, selector-list, and common attribute selectors.
- Selected pseudo-classes including `:root`, `:scope`, `:checked`, `:disabled`, `:hover`, `:focus`, `:focus-visible`, `:focus-within`, `:first-child`, `:last-child`, `:not(...)`, `:has(...)`, `:nth-child(...)`, and `:nth-of-type(...)`.
- Specificity, source order, inline styles, `!important`, inheritance, custom properties, and `var(...)`.
- Width-based and height-based `min`/`max` media queries used during layout.
- Block and inline formatting, inline blocks, flex rows and columns, CSS grid tracks and named areas, and table layout.
- Fixed, absolute, relative, and sticky positioning used by the current layout engine.
- Intrinsic sizing, `min-content`, `max-content`, percentages, `px`, `em`, `rem`, `vw`, `vh`, and a limited set of `calc()`, `min()`, `max()`, and `clamp()` expressions.
- Margins, padding, gaps, borders, border radius, box sizing, overflow, scrollbars, scrollbar gutter, z-order, and pointer hit testing.
- Font family, size, style, weight, line height, alignment, decoration, white-space handling, tabs, clipping, and ellipsis behavior needed by the tested UI layouts.
- Hex, named, `rgb()`, `rgba()`, `currentColor`, color mixing used by the engine, solid backgrounds, basic linear/radial gradients, and box shadows.
- Basic translate, scale, and rotate transforms.
- `::before` and `::after` generated boxes.
- Numeric CSS transitions, timing functions, delays, selected discrete transitions, and transition-driven relayout.
- Basic SVG shapes and paths, including `symbol`/`use` references used by UI icons.
- Direct2D painting and DirectWrite text measurement/rendering.

### JavaScript runtime

- `let`, `const`, `var`, functions, function expressions, arrow functions, closures, lexical arrow `this`, rest parameters, default parameters, destructuring, and argument spread.
- Class declarations with constructors, instance methods, static methods/fields, `new`, and `Function.bind` for the tested patterns.
- Objects, arrays, strings, numbers, booleans, `null`, `undefined`, regular expressions, and template literals.
- Property/index access, assignment, arithmetic, comparison, logical, nullish, bitwise, shift, ternary, comma, `in`, `typeof`, `void`, and `delete` operations.
- Object and array spread.
- `if`/`else`, `switch`, C-style `for`, `for...of`, `for...in`, `while`, `do...while`, `break`, `continue`, `return`, `throw`, and `try`/`catch`/`finally`.
- A practical subset of `Array`, `Object`, `String`, `Number`, `Math`, `Date`, `Map`, `Set`, `RegExp`, `JSON`, and selected `Intl` behavior.
- Promise states and chaining, thenable adoption, `then`, `catch`, `finally`, `resolve`, `reject`, `all`, `race`, `allSettled`, and `any` for the tested inputs.
- Microtasks, `queueMicrotask`, unhandled/handled rejection events, async functions, and `await`.
- `setTimeout`, `clearTimeout`, `setInterval`, `clearInterval`, `requestAnimationFrame`, and `cancelAnimationFrame`.
- Compatibility subsets of `performance.now`, `structuredClone`, `CSS.escape`, `URLSearchParams`, `localStorage`, `location.search`, `getComputedStyle`, and `matchMedia`.
- DOM bindings and the bidirectional `window.chrome.webview` host-message bridge.
- Host-backed `fetch(...).json()` for application resources supplied through `SetResourceLoader()`.

### Input and accessibility

- Custom editing for text inputs, textareas, and `contenteditable` without creating a child Win32 `EDIT` control.
- Caret movement, selection, insertion, deletion, copy/cut/paste, undo/redo, and DOM selection synchronization.
- IMM32 composition start/update/commit/cancel, inline composition display, and candidate-window positioning, including tested Korean input behavior.
- Tab and Shift+Tab focus order, `:focus-visible`, button/link keyboard activation, and common ARIA menu keyboard navigation.
- Server-side UI Automation through `WM_GETOBJECT`.
- Accessible names and states from common HTML and ARIA attributes.
- Invoke, Value, Toggle, ExpandCollapse, Selection, and SelectionItem UI Automation patterns.
- Stable automation IDs from `data-automation-id`, `id`, or `name`, with a deterministic DOM path as a fallback.

## Not implemented or intentionally limited in 0.2

Anything not listed above should be considered unsupported until it has both an implementation and a regression test.

### Browser platform and security

- TWebFrame has no browser process model, renderer sandbox, site isolation, origin model, permissions model, certificate validation, CSP, CORS, or other browser security boundary.
- There is no built-in HTTP/HTTPS stack, DNS, cookies, cache, download manager, browser history, or general URL navigation.
- `fetch()` does not access the network. It only calls the host resource loader and currently exposes the response fields and JSON path required by local application pages.
- External stylesheets, scripts, iframe documents, and fetched data must be supplied by the host.
- `localStorage` is an in-runtime compatibility object and is not persistent browser storage.
- Service workers, web workers, WebSocket, XMLHttpRequest, IndexedDB, Cache Storage, and the broader storage/network platform are not implemented.
- This runtime must not be used to execute untrusted web content.

### HTML and DOM gaps

- The HTML parser is not the WHATWG parsing algorithm and does not implement its complete error recovery, document modes, namespaces, or full node model.
- Shadow DOM, Custom Elements, slots, MutationObserver, ResizeObserver, IntersectionObserver, Range, Selection, and the complete browser event model are not implemented.
- Canvas, WebGL, media playback, image decoding, printing, and browser-style navigation are not implemented.
- Forms do not provide a complete browser submission, validation, autofill, or credential workflow.
- Iframes are a local host-resource feature, not isolated browsing contexts with web-origin semantics.

### CSS and rendering gaps

- CSS parsing, cascade, computed values, layout, and painting cover only the properties and values required by the current test suite.
- The implementation is not expected to match browser behavior for all flexbox, grid, table, intrinsic sizing, fragmentation, bidi, writing-mode, or typography edge cases.
- Transforms, gradients, SVG, transitions, filters, compositing, and animations are limited; there is no complete CSS animation or GPU compositing system.
- Browser font fallback, shaping, rasterization, subpixel behavior, and platform control appearance may differ from Chromium and other browsers.
- There is no standards test-suite conformance claim.

### JavaScript and Web API gaps

- The language is an ES-like tested subset, not a complete ECMAScript implementation.
- ES modules (`import`/`export`), dynamic import, generators, class inheritance, instance class fields, proxies, symbols, weak collections, typed arrays, and many reflection/meta-programming features are not implemented.
- The exposed `BigInt` compatibility path is number-backed and is not arbitrary-precision ECMAScript `BigInt`.
- Regular expressions, `Intl`, `Date`, `JSON`, `URLSearchParams`, `structuredClone`, storage, and other built-ins provide only the behavior exercised by the current runtime and tests.
- Promise combinators and iterable handling are narrower than the ECMAScript specification.
- `matchMedia()` is currently a compatibility placeholder; CSS media queries are evaluated by the stylesheet engine, but the JavaScript object does not provide full query evaluation or change events.
- `eval`, the `Function` constructor, debugging protocols, source maps, and JIT compilation are not provided.

### Platform and compatibility

- Version 0.2 targets Windows and uses Win32, Direct2D, DirectWrite, IMM32, and UI Automation.
- The documented and continuously exercised build is Visual Studio 2019 with the v142 toolset, C++17, and x64.
- The public API and behavior may change while the project remains in the 0.x series.

## Build

Open `TWebFrame.sln` in Visual Studio 2019 and build `x64 / Debug` or `x64 / Release`.

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe' `
    TWebFrame.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

The root solution contains:

- `TWebFrame` — reusable static library.
- `TWebFrameTests` — self-contained regression tests for the shared HTML, DOM, CSS, layout, JavaScript, input, and accessibility paths.

## Test

```powershell
.\bin\x64\Release\TWebFrameTests.exe
```

The tests construct their own fixtures and do not require application-specific HTML, CSS, or JavaScript files. At version 0.2, the regression suite is the most precise executable definition of supported behavior.

For screenshot comparisons:

```powershell
.\tools\Compare-Screenshots.ps1 reference.png twebframe.png -Tolerance 24
```

## Diagnostics

- `View::DumpLayoutJson()` returns element tags, IDs, and integer layout bounds.
- `View::DumpAccessibilityJson()` returns the names, automation IDs, control types, states, and screen bounds used by the UI Automation provider.
- `View::LastError()` returns the most recent load or execution error.

These APIs are intended for native-host regression tests and for diagnosing shared engine behavior.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Detailed supported subset](docs/SUPPORTED.md)
