# TWebFrame Version History

This document records user-visible changes cumulatively by released version. Add each new release above the previous entries.

## 0.8 - 2026-09-23

### Added

- Added shared pointer transition events with CSS-pixel coordinates, related targets, and 100%/150% DPI regression coverage.
- Added DOM range and selection support plus `document.execCommand('insertHTML')` for block-aware editable HTML insertion.
- Added native title tooltips and focused pointer, editing-command, and tooltip regression projects under `TWebFrame/tests`.

### Improved

- Extended the common JavaScript runtime with `Number.isInteger()` and Unicode Letter property escapes used by multilingual regular expressions.
- Made array/string index parsing and numeric conversion exception-free during normal JavaScript property access and conversion.

### Fixed

- Preserved JavaScript-triggered layout invalidation across hover transitions and kept pointer hit testing DPI-independent.
- Fixed editable table insertion at stored selections, including empty editors, paragraph splitting, and a trailing editable paragraph.
- Prevented supported Unicode property expressions from raising first-chance `std::regex_error` exceptions.

## 0.7 - 2026-09-23

### Added

- Added a reusable Canvas 2D subset with paths, fills, strokes, gradients, text, transforms, and DPI-independent compositing.
- Added common-engine regression coverage for form controls at 100% and 150% DPI, canvas rendering, table spans, inline events, and heap/GUI-resource teardown.

### Improved

- Extended shared JavaScript, DOM, CSS, and layout behavior used by application navigation and settings views, including named functions, location/dialog behavior, dynamic script execution, SVG data-URL backgrounds, and table spanning.
- Matched standard checkbox and radio control states, including `appearance: none`, checked, unchecked, disabled, and indeterminate rendering without page- or ID-specific rules.

### Fixed

- Released cyclic JavaScript graphs, timers, callbacks, node wrappers, canvas contexts, and detached-node listener entries during reload and teardown.
- Disconnected UI Automation providers and released accessibility, layout-thread, render-target, and child-frame resources when a view is destroyed.

## 0.6 - 2026-09-22

### Added

- Added incremental DOM ID, tag, and class indexes with mutation-aware updates and document-order-preserving selector queries.
- Added regression and benchmark coverage for selector indexing, mutation invalidation, scroll-only updates, and large-document rendering.

### Improved

- Reduced repeated selector matching, query parsing, class-token scanning, attribute normalization, and inline event compilation.
- Reused layout, text, brush, and SVG geometry work where safe, and limited paint work to invalidated regions.
- Narrowed accessibility live-region updates and scroll-only mutations to the affected state without forcing a complete layout.

## 0.5 - 2026-09-21

### Added

- Added shared listener registration/removal behavior for capture, once, and passive options across window, document, element, and host-message targets.
- Added horizontal overflow geometry, `scrollLeft`/`scrollWidth`, `scroll()`/`scrollTo()`/`scrollBy()`, horizontal scrollbar painting and interaction, and horizontal wheel routing.
- Added focused horizontal-scroll regression coverage at 100% and 150% DPI scaling.

### Improved

- Extended flex wrapping to column, reverse-direction, `flex-flow`, and cross-axis `wrap-reverse` layouts.
- Kept scrollbar geometry and pointer conversion in CSS DIPs so rendering and interaction remain stable across monitor DPI changes.

## 0.3 - 2026-09-21

### Added

- Added multiline `flex-wrap` and `wrap-reverse` layout with per-line sizing, alignment, gaps, and intrinsic-height calculation.
- Added inline HTML event-attribute dispatch with the current element as `this`, an `event` argument, bubbling behavior, and `return false` cancellation.
- Added pointer-drag text selection for editable controls, including normalized DOM selection ranges and DPI-aware clipboard workflows.

### Improved

- Improved sticky table-header painting, hit testing, and scrollbar interaction by including zero-index stacking contexts in non-negative stacking order.
- Added focused regression projects for flex wrapping, inline events, sticky table headers, and pointer text selection.

## 0.2 - 2026-09-20

### Added

- Added `window.twebframe` as the native host-message bridge while retaining `window.chrome.webview` compatibility.
- Added `place-content` shorthand handling and grid content distribution for `center`, `end`, `space-between`, `space-around`, and `space-evenly`.
- Added main-axis and cross-axis automatic margin handling for flex items.
- Added native directory metadata for file-drop input, represented as `application/x-directory` with a size of zero.

### Improved

- Improved CSS grid automatic minimums, stretched auto tracks, and fractional-track sizing.
- Improved inline line-height, baseline placement, and table-cell vertical alignment.
- Improved Korean font fallback so missing Hangul glyph runs use an installed Korean UI font across more primary font families.
- Improved placeholder rendering with the default user-agent color and support for author-defined placeholder opacity.

## 0.1 - 2026-09-20

### Added

- Introduced the experimental Windows-native HTML UI runtime and its public `TWebFrame::View` API.
- Added the initial HTML/DOM, CSS, layout, JavaScript, input, accessibility, diagnostics, and native host-integration subsets.
- Exposed `VersionMajor`, `VersionMinor`, and `VersionString` public version constants.
