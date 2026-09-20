# TWebFrame Version History

This document records user-visible changes cumulatively by released version. Add each new release above the previous entries.

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
