#pragma once

#include "CSS.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

struct RasterImageFrame;
struct RasterImage;

struct LayoutRect {
    float x = 0, y = 0, width = 0, height = 0;
    bool Contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

struct LayoutBox {
    std::shared_ptr<Node> node;
    // Generated CSS boxes keep their originating DOM node separate so they
    // can participate in layout/painting without becoming DOM children.
    std::shared_ptr<Node> generatedFrom;
    std::wstring pseudo;
    LayoutBox* parent = nullptr;
    ComputedStyle style;
    LayoutRect rect;
    LayoutRect content;
    std::vector<std::unique_ptr<LayoutBox>> children;
    // Painting and hit testing use the same CSS stacking order repeatedly.
    // Cache it after layout instead of rebuilding and sorting a temporary
    // vector for every frame and pointer event.
    std::vector<LayoutBox*> paintChildren;
    std::vector<LayoutBox*> nonNegativeStackingContexts;
    std::vector<LayoutBox*> verticallyOrderedChildren;
    std::vector<LayoutBox*> overlayChildren;
    bool visible = true;
    bool preserveLeadingWhitespace = false;
    bool preserveTrailingWhitespace = false;
    bool containsSticky = false;
    bool stickyFlowYValid = false;
    float stickyFlowY = 0.0f;
    float scrollWidth = 0.0f;
    float scrollHeight = 0.0f;
    float appliedScrollLeft = 0.0f;
    float appliedScrollTop = 0.0f;
    // Synthetic line fragments retain their offset in the originating DOM
    // text node so pointer/caret geometry maps back to DOM offsets.
    size_t textSourceOffset = 0;
    std::wstring textLayoutKey;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    mutable bool naturalWidthValid = false;
    mutable float naturalWidth = 0.0f;
    mutable bool minimumWidthValid = false;
    mutable float minimumWidth = 0.0f;
    mutable bool naturalHeightValid = false;
    mutable float naturalHeightReference = 0.0f;
    mutable float naturalHeight = 0.0f;
};

struct StyleTransition {
    std::wstring property;
    std::wstring from;
    std::wstring to;
    float elapsedMs = 0.0f;
    float durationMs = 0.0f;
    float delayMs = 0.0f;
    float x1 = 0.25f;
    float y1 = 0.1f;
    float x2 = 0.25f;
    float y2 = 1.0f;
    bool discrete = false;
};

class LayoutEngine {
public:
    using RasterImageResolver = std::function<std::shared_ptr<RasterImage>(const std::wstring&)>;

    LayoutEngine(Document& document, StyleSheet& styleSheet);
    ~LayoutEngine();
    void SetRasterImageResolver(RasterImageResolver resolver);
    void Layout(float width, float height, float deviceScale = 1.0f);
    // Reuse the current style/layout tree for viewport-only changes. A media
    // query boundary crossing automatically falls back to a full rebuild.
    void Relayout(float width, float height, float deviceScale = 1.0f);
    void Paint(ID2D1RenderTarget* target, IDWriteFactory* writeFactory,
               const LayoutRect* dirtyBounds = nullptr);
    bool ScrollAt(float x, float y, float wheelDelta,
                  std::shared_ptr<Node>* scrolledNode = nullptr,
                  bool horizontal = false);
    bool SyncScroll(const std::shared_ptr<Node>& node);
    bool BeginScrollbarInteraction(float x, float y, std::shared_ptr<Node>& dragNode,
                                   float& dragOffset, bool& horizontal);
    bool BeginScrollbarInteraction(float x, float y, std::shared_ptr<Node>& dragNode,
                                   float& dragOffset);
    bool DragScrollbar(const std::shared_ptr<Node>& node, float x, float y,
                       float dragOffset, bool horizontal);
    bool DragScrollbar(const std::shared_ptr<Node>& node, float y, float dragOffset);
    bool Restyle(const std::shared_ptr<Node>& node);
    std::shared_ptr<Node> HitTest(float x, float y) const;
    bool HitTestText(const std::shared_ptr<Node>& scope, float x, float y,
                     std::shared_ptr<Node>& textNode, size_t& textOffset);
    bool TextRangeRects(const std::shared_ptr<Node>& textNode, size_t textStart,
                        size_t textLength, std::vector<LayoutRect>& rects);
    bool VerticalCaretPosition(const std::shared_ptr<Node>& scope,
                               const std::shared_ptr<Node>& currentNode,
                               size_t currentOffset, float preferredX, bool upward,
                               std::shared_ptr<Node>& targetNode, size_t& targetOffset);
    bool TextCaretRect(const std::shared_ptr<Node>& textNode, size_t textOffset,
                       LayoutRect& caretRect);
    std::wstring DumpJson() const;
    const LayoutBox* Root() const { return root_.get(); }
    const LayoutBox* BoxFor(const std::shared_ptr<Node>& node) const;
    bool VisualBounds(const std::shared_ptr<Node>& node, LayoutRect& bounds) const;
    bool HasActiveTransitions() const;
    bool AdvanceTransitions(float milliseconds);
    void ClearTransitions();
    void DiscardDeviceResources();

private:
    std::unique_ptr<LayoutBox> Build(const std::shared_ptr<Node>& node,
                                     const ComputedStyle* parentStyle,
                                     std::uint64_t parentContext = 1469598103934665603ull,
                                     size_t siblingIndex = 0, size_t siblingCount = 0,
                                     const std::shared_ptr<Node>& previousElement = {});
    void LayoutBoxTree(LayoutBox& box, const LayoutRect& available, bool forcedSize = false,
                       bool definiteWidth = true, bool definiteHeight = true);
    void FinalizeScroll(LayoutBox& box);
    void LayoutBlock(LayoutBox& box, bool definiteHeight = true);
    void LayoutFlex(LayoutBox& box);
    void LayoutGrid(LayoutBox& box, bool definiteWidth, bool definiteHeight);
    void LayoutTable(LayoutBox& box);
    void UpdateTraversalMetadata(LayoutBox& box);
    void UpdateStackingContexts(LayoutBox& scope);
    void UpdateTopLayer();
    void PaintStackingContext(ID2D1RenderTarget* target, IDWriteFactory* factory,
                              LayoutBox& box, const LayoutRect& clipBounds);
    void PaintDialogBackdrop(ID2D1RenderTarget* target, const LayoutBox& dialog,
                             const LayoutRect& clipBounds);
    void PaintBox(ID2D1RenderTarget* target, IDWriteFactory* factory, LayoutBox& box,
                  const LayoutRect& clipBounds,
                  const std::vector<LayoutBox*>* deferredContexts = nullptr);
    std::shared_ptr<Node> HitTestStackingContext(const LayoutBox& box, float x, float y) const;
    std::shared_ptr<Node> HitTestBox(const LayoutBox& box, float x, float y) const;
    bool ScrollBox(LayoutBox& box, float x, float y, float wheelDelta,
                   std::shared_ptr<Node>* scrolledNode, bool horizontal);
    bool BeginScrollbarBox(LayoutBox& box, float x, float y,
                           std::shared_ptr<Node>& dragNode, float& dragOffset,
                           bool& horizontal);
    void DumpBox(const LayoutBox& box, std::wstring& output, bool& first) const;
    bool RestyleBox(LayoutBox& box, const ComputedStyle* parentStyle);
    void ApplyTransitions(LayoutBox& box);
    void RefreshTransitionFrame(LayoutBox& box);
    void InvalidateMeasurements(LayoutBox& box);
    ID2D1SolidColorBrush* SolidBrush(ID2D1RenderTarget* target, unsigned int color);
    ID2D1SolidColorBrush* SolidBrush(ID2D1RenderTarget* target, const D2D1_COLOR_F& color);
    void EnsureGeometryResources(ID2D1RenderTarget* target);

    Document& document_;
    StyleSheet& styleSheet_;
    std::unique_ptr<LayoutBox> root_;
    FastMap<const Node*, LayoutBox*> boxIndex_;
    FastMap<std::uint64_t, ComputedStyle> styleCache_;
    FastMap<const Node*, ComputedStyle> transitionTargets_;
    FastMap<const Node*, std::vector<StyleTransition>> transitions_;
    std::uint64_t styleCacheVersion_ = 0;
    float viewportWidth_ = 0;
    float viewportHeight_ = 0;
    float deviceScale_ = 1.0f;
    std::vector<LayoutBox*> modalBoxes_;
    const LayoutBox* paintingTopLayer_ = nullptr;
    const LayoutBox* canvasBackgroundBox_ = nullptr;
    ID2D1RenderTarget* brushCacheTarget_ = nullptr;
    FastMap<unsigned int, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushCache_;
    ID2D1Factory* geometryFactory_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> selectArrowGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> verticalArrowGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> horizontalArrowGeometry_;
    FastMap<std::wstring, Microsoft::WRL::ComPtr<ID2D1PathGeometry>> svgGeometryCache_;
    FastMap<std::wstring, std::shared_ptr<Node>> svgBackgroundCache_;
    RasterImageResolver rasterImageResolver_;
    ID2D1RenderTarget* imageBitmapCacheTarget_ = nullptr;
    FastMap<const RasterImageFrame*, Microsoft::WRL::ComPtr<ID2D1Bitmap>> imageBitmapCache_;
    ID2D1RenderTarget* shadowBitmapCacheTarget_ = nullptr;
    FastMap<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> shadowBitmapCache_;
};

} // namespace TWebFrame::Internal
