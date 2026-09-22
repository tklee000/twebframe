# Version 0.7 Supported Subset

This document records the behavior currently implemented and exercised by the TWebFrame regression suite. It is not a claim of complete HTML, CSS, DOM, Web API, accessibility, or ECMAScript conformance.

TWebFrame 0.7 is intended for trusted HTML user interfaces owned by a C or C++ Windows application. New behavior is implemented as a reusable rule in the common JavaScript, DOM, CSS, layout, input, or accessibility engine rather than as an exception for a particular page.

Unless a feature is listed here and covered by a test, applications should treat it as unsupported.

## HTML and DOM

### Tree and parsing

- Document, element, and text nodes.
- Common document and sectioning elements such as `html`, `head`, `body`, `main`, `section`, `header`, `footer`, `nav`, `article`, and `aside`.
- Common phrasing and grouping elements such as `div`, `span`, `p`, `br`, `hr`, `a`, `b`, `strong`, `i`, `em`, `small`, `code`, `pre`, lists, and headings.
- Common form and table elements such as `button`, `input`, `textarea`, `select`, `option`, `datalist`, `label`, `form`, `table`, `thead`, `tbody`, `tr`, `th`, and `td`.
- Basic `dialog`, `template`, `iframe`, and SVG element handling used by the test suite.
- Quoted, unquoted, boolean, and ordinary attributes.
- Common character references, including non-breaking space.
- Paragraph defaults and tested omitted-closing-tag behavior.
- HTML fragment parsing for `innerHTML` and dynamic replacement.
- Extraction of inline/external styles and scripts during view loading.

The parser does not implement the complete WHATWG state machine, error recovery, namespaces, document modes, or full node taxonomy.

### Queries and mutation

- `document.getElementById()`
- `document.getElementsByName()`
- `document.querySelector()` and `document.querySelectorAll()`
- Element-scoped `querySelector()` and `querySelectorAll()`
- `document.createElement()`
- `appendChild()`, `append()`, `insertBefore()`, `replaceChildren()`, `replaceWith()`, and `remove()`
- `closest()`, `matches()`, and `contains()`
- `setAttribute()`, `getAttribute()`, `hasAttribute()`, and `removeAttribute()`
- `innerHTML`, `innerText`, and `textContent`
- `children`, `childElementCount`, `firstElementChild`, `lastElementChild`, and `nextElementSibling`
- `template.content`
- `classList.add()`, `remove()`, and `toggle()`
- `style.*` and `dataset.*`

### Element and control state

- `value`, `checked`, `disabled`, `files`, selection offsets, and table `cells` for the tested elements.
- `focus()`, `document.activeElement`, and `scrollTo()`.
- `setSelectionRange()` plus `selectionStart` and `selectionEnd` for supported editors.
- `dialog.open`, `show()`, `showModal()`, and `close()`.
- `form.requestSubmit()`.
- Select/option value updates and datalist suggestion selection.
- File metadata (`name`, `size`, and `type`) and native file-drop data.
- `getBoundingClientRect()` and selected client/scroll geometry properties.
- Basic iframe `contentWindow`, load events, and parent/frame messaging for host-loaded documents.

### Events

- Document, window, WebView bridge, and element event listeners, including removal, capture, once, and passive options.
- Inline HTML event attributes with element-bound `this`, the current `event`, bubbling, and `return false` cancellation.
- Capture, target, and bubbling phases for the implemented events.
- `preventDefault()`, `stopPropagation()`, `stopImmediatePropagation()`, and `composedPath()` for the current tree model.
- Tested event families include `DOMContentLoaded`, `load`, `click`, `dblclick`, `pointerdown`, compatible mouse input, keyboard input, `focusin`, `focusout`, `beforeinput`, `input`, `change`, `scroll`, `drop`, and `message`.
- Tested metadata includes `key`, `button`, `detail`, modifier keys, input data/type, composition state, target, current target, and default-prevented state.

Shadow DOM, Custom Elements, browser observer APIs, the full Event constructor family, and full composed-tree semantics are not implemented.

## CSS selectors and cascade

### Selectors

- Type, universal, ID, class, and compound selectors.
- Descendant, child (`>`), adjacent-sibling (`+`), and general-sibling (`~`) combinators.
- Comma-separated selector lists.
- Attribute presence and `=`, `^=`, `$=`, `*=`, `~=`, and `|=` matching, including the tested ASCII-insensitive flag.
- `:root`, `:scope`, `:checked`, `:indeterminate`, `:disabled`, `:hover`, `:focus`, `:focus-visible`, and `:focus-within`.
- `:first-child`, `:last-child`, `:not(...)`, `:has(...)`, `:nth-child(...)`, and `:nth-of-type(...)` for tested patterns.
- `::before` and `::after` generated boxes.

### Cascade

- Selector specificity and source order.
- Inline declarations and `!important`.
- Inherited properties used by the renderer.
- CSS custom properties and nested `var(...)` resolution for tested values.
- Shorthand expansion used by the current style and layout properties.
- Viewport `min-width`, `max-width`, `min-height`, and `max-height` media conditions.

JavaScript `matchMedia()` is only a compatibility placeholder in version 0.7 and does not mirror the stylesheet engine's complete media-query state.

## CSS layout and paint

### Formatting and sizing

- Block, inline, inline-block, inline-flex, and inline-grid participation used by the tested layouts.
- Collapsible and preserved whitespace, line breaks, nowrap behavior, tabs, clipping, and ellipsis.
- Flex row/column and reverse-direction layout, tested `flex-flow`, `flex-wrap`/`wrap-reverse`, gaps, common alignment/justification values, baseline alignment, and automatic minimum-size behavior.
- Explicit and implicit grid tracks, `fr`, `minmax()`, `repeat()`, tested `auto-fit`, named grid areas, spans, and gaps.
- Automatic and fixed table layout used by the tests.
- Width, height, minimum/maximum sizes, intrinsic `min-content`/`max-content`, percentages, and aspect-independent control sizing.
- Margin, padding, border, border radius, gap, and `box-sizing`.
- Absolute, fixed, relative, and sticky positioning with tested inset behavior.
- Overflow clipping, `overflow: auto/scroll`, vertical and horizontal mouse-wheel scrolling, `scrollLeft`/`scrollWidth`, scrollbar dragging, thin scrollbar styling, and stable scrollbar gutters.
- Z-index stacking contexts, overflow-visible hit testing, and `pointer-events: none`.
- Parent-window sizing hit forwarding and Windows resize cursors.

### Values and paint

- `px`, `%`, `em`, `rem`, `vw`, and `vh`.
- Tested `calc()`, `min()`, `max()`, and `clamp()` length expressions.
- Three-, four-, six-, and eight-digit hexadecimal colors.
- Named colors and comma/space forms of `rgb()` and `rgba()` used by the tests.
- `currentColor`, tested color mixing, solid backgrounds, multiple background fallback, basic linear/radial gradients, and box shadows.
- Font family, size, style, weight, line height, `font` shorthand, text alignment, text decoration, and white-space handling.
- Basic translate, scale, and rotate transforms.
- Opacity and visibility.
- Basic SVG shapes and paths plus `symbol`/`use` references.
- Direct2D box/control/SVG painting and DirectWrite text rendering.

### Transitions

- Numeric interpolation for compatible tested CSS values, including lengths, opacity, transforms, and grid tracks.
- `ease`-family and cubic-bezier timing functions.
- Transition delays and the tested delayed `visibility` behavior.
- Forward and reverse transitions with layout-tree reuse where possible.

There is no complete keyframe animation, transform-composition, filter, compositor, or GPU animation system. Complex gradient/SVG/transition behavior remains limited.

## JavaScript language

### Syntax and execution

- `let`, `const`, and `var` declarations.
- Function declarations and expressions, arrow functions, closures, lexical arrow `this`, and `arguments`.
- Rest parameters, argument spread, default parameters, and tested destructuring patterns.
- Classes with constructors, instance methods, static methods/fields, `new`, and bound methods.
- Object and array literals, sparse array items used by tests, object/array spread, strings, numbers, booleans, `null`, and `undefined`.
- Numeric separators, regular-expression literals, and template literals.
- Property/index access and assignment.
- Arithmetic, comparison, logical, nullish, bitwise, shift, compound-assignment, unary, ternary, comma, `in`, `typeof`, `void`, and `delete` operators used by the tests.
- `if`/`else`, `switch` with fallthrough, C-style `for`, `for...of`, `for...in`, `while`, and `do...while`.
- `break`, `continue`, `return`, block scope, `throw`, and `try`/`catch`/`finally` with abrupt-completion handling.
- Async functions and resumable `await` execution.

ES modules, dynamic import, generators, class inheritance, instance class fields, proxies, symbols, weak collections, typed arrays, and a complete reflection/meta-object system are not implemented.

### Built-ins

- Common array operations including `forEach`, `filter`, `every`, `some`, `map`, `find`, `findIndex`, `at`, `sort`, `includes`, `join`, `push`, and `unshift`.
- `Array.from()` and `Array.isArray()` for the supported inputs.
- `Map`, `Set`, and their tested iteration methods.
- Common string conversion, case, search, split, replace, indexing, and `localeCompare` behavior.
- Tested regular-expression matching/replacement and replacement capture references.
- Common number conversion/formatting plus `Number.isFinite()` and `Number.isNaN()`.
- The tested `Math` functions.
- Date creation, parsing, arithmetic, `Date.now()`, `getTime()`, `toISOString()`, and selected local-time accessors.
- Limited `Intl.RelativeTimeFormat` and `Intl.DateTimeFormat` behavior.
- `JSON.parse()` and `JSON.stringify()` compatibility paths.
- `Object.freeze()` and `hasOwnProperty()` for tested object patterns.
- `CSS.escape()`, `getComputedStyle()`, `structuredClone()`, `URLSearchParams`, `localStorage`, `location.search`, and `performance.now()` compatibility subsets.
- Standard error objects required by the VM's tested exception paths.

These built-ins are not complete specification implementations. In particular, the exposed `BigInt` path is number-backed, storage is not persistent, and internationalization/regular-expression behavior is narrower than a browser engine.

### Promises and scheduling

- Pending, fulfilled, and rejected promise states.
- Thenable adoption and chaining.
- `then()`, `catch()`, and `finally()`.
- `Promise.resolve()`, `reject()`, `all()`, `race()`, `allSettled()`, and `any()` for tested array inputs.
- Recursive microtask draining at task boundaries.
- `queueMicrotask()`.
- Unhandled and later-handled rejection events.
- Real delayed/cancelable timeouts and intervals integrated with the view.
- Animation-frame callback scheduling and cancellation.

Promise combinators do not accept every ECMAScript iterable pattern, and scheduling does not attempt to reproduce a complete browser event loop.

### Host and Web API compatibility

- DOM host objects and event dispatch.
- `window.chrome.webview.postMessage()` and message listeners.
- `PostWebMessageAsJson()` and `PostWebMessageAsString()` from C++.
- Basic parent/iframe `postMessage()` for host-loaded child documents.
- Host-backed `fetch(...).json()`.

`fetch()` does not perform network I/O. XMLHttpRequest, WebSocket, workers, service workers, cookies, browser cache, IndexedDB, and origin/CORS behavior are not implemented.

## Rendering and controls

- Direct2D backgrounds, borders, shadows, gradients, controls, SVG paint, and SVG data-URL backgrounds.
- DirectWrite text measurement and drawing.
- Checkbox, radio, button, input, textarea, select, datalist, and `contenteditable` UI needed by the tests, including native checked/unchecked/indeterminate states and `appearance: none`.
- A Canvas 2D subset with fill/stroke rectangles and paths, gradients, text, transforms, save/restore, and intrinsic backing-store scaling.
- Pointer hit testing that follows layout, clipping, transforms, overflow, pointer-events, and stacking order for tested cases.
- Dynamic restyle and relayout after DOM or style mutation.
- Native select/datalist popup behavior integrated with DOM input/change events.

Image decoding, Canvas pixel/image APIs, WebGL, video/audio, printing, browser plug-ins, and general replaced-element rendering are not implemented.

## Keyboard and text input

- Character input, navigation, pointer-drag selection, deletion, and line editing for the supported editors.
- Windows clipboard copy, cut, and paste.
- Undo and redo.
- Custom caret and selection geometry without a child `EDIT` window.
- IMM32 composition start, update, commit, and cancel.
- Inline display of intermediate composition text and candidate-window positioning.
- Positive-`tabindex` ordering followed by DOM order for sequential focus navigation.
- Exclusion of disabled, negative-tabindex, and non-visible elements from sequential focus.
- `:focus-visible`, bubbling focus events, and keyboard activation for common buttons, links, and ARIA controls.
- Tested ARIA menu navigation with arrows, Home/End, Escape, Enter, and Space.

Text services other than the current IMM32 boundary and complete browser editing/selection specifications are outside version 0.7.

## Accessibility

- Server-side UI Automation fragment provider returned through `WM_GETOBJECT`.
- Screen-coordinate bounds from the shared layout tree.
- Common HTML and ARIA roles, labels, label references, live settings, and checked/disabled/expanded/selected states.
- Invoke, Value, Toggle, ExpandCollapse, Selection, and SelectionItem patterns.
- Focus, property, and live-region notifications used by the current controls.
- Stable automation IDs from `data-automation-id`, `id`, or `name`, with a deterministic DOM path fallback.
- `View::DumpAccessibilityJson()` for native-host regression testing.

The provider is a practical subset for the implemented controls, not a complete mapping of every HTML Accessibility API or UI Automation pattern.

## Platform scope

- Windows desktop only.
- Win32 child-window hosting.
- Direct2D and DirectWrite rendering.
- IMM32 input integration.
- UI Automation accessibility.
- C++17 and the Visual Studio v142 toolset in the documented build.

Cross-platform rendering, non-Windows window systems, mobile platforms, and browser embedding are not part of version 0.7.
