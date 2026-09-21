# TWebFrame Architecture

TWebFrame 0.5 is a Windows-native HTML UI runtime for C and C++ hosts. It follows a browser-like processing pipeline within a deliberately smaller scope:

1. `DOM.cpp` tokenizes HTML and builds the document tree, attributes, entities, and fragments.
2. `CSS.cpp` parses rules, matches selectors, calculates specificity and cascade order, resolves inheritance and custom properties, and tracks media-query state.
3. `JavaScript.cpp` lexes source, compiles it to bytecode, runs it in a stack VM, schedules promises and tasks, and exposes DOM and host bindings.
4. `Layout.cpp` builds the layout tree, calculates block/flex/grid/table geometry, paints it, and performs hit testing and scrolling.
5. `View.cpp` owns the TWebFrame child `HWND`, Direct2D/DirectWrite resources, input routing, DOM editing, events, iframe views, and host callbacks.
6. `TextInput.cpp` translates Win32 IMM32 messages into renderer-independent composition callbacks.
7. `Accessibility.cpp` implements the `WM_GETOBJECT` UI Automation fragment tree, properties, and control patterns.

This architecture borrows the ordering of a browser engine, but it is not a browser implementation and does not claim full web-platform conformance.

## Public boundary

`TWebFrame::View` is the supported C++ boundary. A host creates a view for a parent window, provides its bounds, loads HTML, and optionally installs message, load, and resource callbacks.

The public API owns the following responsibilities:

- Create and destroy the child window.
- Resize or show/hide the rendered surface.
- Navigate to a host file or an in-memory HTML string.
- Execute source through the common JavaScript compiler and VM.
- Exchange messages through the `window.chrome.webview` compatibility bridge.
- Resolve external text resources through a host-provided loader.
- Export layout and accessibility diagnostics.

The bridge name is compatible with existing local pages, but no WebView2 component is used.

## Document lifecycle

Page loading is synchronous in version 0.5:

1. Parse the HTML document.
2. Resolve host-provided external stylesheets and scripts.
3. Parse the stylesheet and compile/execute scripts.
4. Build style and layout state.
5. Dispatch `DOMContentLoaded`.
6. Notify the C++ load handler.

DOM and inline-style mutations invalidate style or layout state. The next paint recalculates the required shared engine state. Viewport-only size changes can reuse the layout and text caches when no media-query boundary is crossed. Dirty-subtree layout and browser-scale incremental rendering are not goals of version 0.5.

## DOM model

The core tree contains document, element, and text nodes. Element state stores attributes, inline style, children, form/control state, selection offsets, scroll position, and interaction flags such as hover and focus.

The same parser is used for initial documents and dynamic fragments such as `innerHTML`. The document maintains an ID index and exposes the selector matcher to DOM APIs and stylesheet evaluation. Comments, document modes, namespaces, and the complete WHATWG node model are outside the current architecture.

## Style and layout

The stylesheet engine compiles selector parts, indexes candidate rules, calculates cascade results, resolves custom properties, and returns a `ComputedStyle` value map. Layout consumes that common result; it does not contain screen-ID-specific style paths.

The layout tree may contain generated boxes that do not become DOM children, including pseudo-element content and anonymous formatting boxes. Each box keeps computed style, border/content geometry, paint order, scroll state, text-layout caches, and optional transition state.

The current layout implementations cover the tested subset of:

- Block and inline formatting.
- Flex row and column layout.
- Explicit and implicit grid tracks and named grid areas.
- Table layout.
- Absolute, fixed, relative, and sticky positioning.
- Intrinsic measurement, overflow, scrolling, and scrollbars.
- Stacking contexts, transforms, transitions, and pointer hit testing.

Painting uses Direct2D for boxes, controls, SVG primitives, and decoration, and DirectWrite for text measurement, shaping, selection, and caret geometry.

## JavaScript execution

All script input is processed by the same lexer, recursive-descent compiler, and bytecode VM. `ExecuteScript()` does not inspect function names and does not route known calls to page-specific C++ handlers.

Compiled functions retain lexical environments for closures. The VM maintains exception handlers and resumable execution frames for `async`/`await`. Promise reactions and `queueMicrotask()` use a microtask queue drained at task boundaries. Timers and animation-frame callbacks are scheduled by the owning view.

DOM objects, events, browser-compatibility objects, and the C++ message bridge are VM host objects. They are a deliberately limited binding layer, not a complete Web API implementation.

## Resource loading

`SetResourceLoader()` gives the C++ host one explicit resource boundary. The engine uses it for page-relative external stylesheets, scripts, iframe documents, and `fetch()` data.

TWebFrame does not contain an HTTP stack. The host decides whether a path maps to a file, an executable resource, generated text, or another trusted application source. The loader returns text and does not create browser origin, CORS, cookie, cache, or credential semantics.

## Text input and IME

TWebFrame does not create a Win32 `EDIT` child for each focused control. The view stores the string, selection, caret, and undo/redo state for `input`, `textarea`, and `contenteditable` nodes, then renders the result through DirectWrite.

Clipboard operations use the Windows clipboard, but text mutation follows the same DOM path as keyboard input. This keeps DOM values, input/change events, layout, and rendered text synchronized.

`TextInput` handles `WM_IME_STARTCOMPOSITION`, `WM_IME_COMPOSITION`, and `WM_IME_ENDCOMPOSITION`. Composition text, attributes, and cursor position are displayed inline before commit. The candidate window is positioned from the caret rectangle. Only the committed result mutates the final DOM value.

## Accessibility

The TWebFrame child window returns a server-side UI Automation fragment provider from `WM_GETOBJECT`. Accessibility data is derived from the same DOM, layout, focus, and control state used for rendering.

Provider calls may arrive off the UI thread. `AccessibilityHost` synchronizes DOM and layout access to the owning window thread. When the window is destroyed, callbacks are disconnected so retained providers cannot access a dead document.

`DumpAccessibilityJson()` exposes the same names, automation IDs, control types, states, and screen bounds as the provider. Explicit `data-automation-id`, `id`, or `name` values are preferred; otherwise the engine creates a deterministic DOM path.

## Shared hot-path storage

DOM attributes, computed style values, JavaScript object properties, event listeners, and style caches use `FastMap.h`. Small maps remain linear and contiguous; larger maps add an index table. This avoids node-per-entry allocation and MSVC debug-iterator overhead on frequently used UI paths.

Debug builds keep PDBs and the Debug CRT while compiling the TWebFrame core with `/O2`. `_ITERATOR_DEBUG_LEVEL=0` is kept consistent across the ABI boundary so large layouts behave consistently between Debug and Release while retaining symbols and breakpoints.

## Generalization rules

- Implement behavior as common parser, runtime, DOM, style, layout, input, or accessibility semantics.
- Do not branch on application function names, element IDs, file names, or screen identity.
- Dynamic markup must use the same fragment parser as other `innerHTML` content.
- JavaScript listeners remain ordinary callable closures stored by the event system.
- Painting and hit testing must use the same layout and stacking state.
- Add a regression test for each newly supported syntax or behavior.

## Diagnostics

`DumpLayoutJson()` exports element tags, IDs, and stable integer pixel bounds. It is intended to separate layout errors from paint differences.

`DumpAccessibilityJson()` exports the UI Automation-facing tree and state. Together, these diagnostics allow a C++ host to test the shared engine without depending only on screenshots.
