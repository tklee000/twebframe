#pragma once

#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

// Canvas coordinates stay in the element's intrinsic pixel space. The layout
// painter maps that space to the CSS content box, while Direct2D applies the
// monitor DPI in the same way as it does for every other DOM element.
struct CanvasTransform {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
};

inline CanvasTransform MultiplyCanvasTransforms(const CanvasTransform& left,
                                                 const CanvasTransform& right) {
    return {
        left.a * right.a + left.b * right.c,
        left.a * right.b + left.b * right.d,
        left.c * right.a + left.d * right.c,
        left.c * right.b + left.d * right.d,
        left.e * right.a + left.f * right.c + right.e,
        left.e * right.b + left.f * right.d + right.f};
}

struct CanvasPoint { float x = 0.0f, y = 0.0f; };

inline CanvasPoint TransformCanvasPoint(const CanvasTransform& transform,
                                        float x, float y) {
    return {x * transform.a + y * transform.c + transform.e,
            x * transform.b + y * transform.d + transform.f};
}

struct CanvasGradientStop {
    float offset = 0.0f;
    std::wstring color;
};

struct CanvasGradient {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    std::vector<CanvasGradientStop> stops;
};

struct CanvasPaint {
    std::wstring color = L"#000000";
    std::shared_ptr<CanvasGradient> gradient;
};

enum class CanvasPathVerb { MoveTo, LineTo, Arc, Close };

struct CanvasPathSegment {
    CanvasPathVerb verb = CanvasPathVerb::MoveTo;
    CanvasPoint point;
    float centerX = 0.0f, centerY = 0.0f, radius = 0.0f;
    float startAngle = 0.0f, endAngle = 0.0f;
    bool counterClockwise = false;
    CanvasTransform transform;
};

struct CanvasDrawingState {
    CanvasPaint fillStyle;
    CanvasPaint strokeStyle;
    float lineWidth = 1.0f;
    std::wstring font = L"10px sans-serif";
    std::wstring textAlign = L"start";
    std::wstring textBaseline = L"alphabetic";
    CanvasTransform transform;
};

inline CanvasPaint SnapshotCanvasPaint(const CanvasPaint& paint) {
    CanvasPaint result = paint;
    if (paint.gradient)
        result.gradient = std::make_shared<CanvasGradient>(*paint.gradient);
    return result;
}

inline CanvasDrawingState SnapshotCanvasState(const CanvasDrawingState& state) {
    CanvasDrawingState result = state;
    result.fillStyle = SnapshotCanvasPaint(state.fillStyle);
    result.strokeStyle = SnapshotCanvasPaint(state.strokeStyle);
    return result;
}

enum class CanvasCommandKind { FillRect, ClearRect, FillPath, StrokePath, FillText };

struct CanvasDrawCommand {
    CanvasCommandKind kind = CanvasCommandKind::FillRect;
    CanvasDrawingState state;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    std::wstring text;
    std::vector<CanvasPathSegment> path;
};

struct CanvasSurface {
    unsigned width = 300;
    unsigned height = 150;
    CanvasDrawingState state;
    std::vector<CanvasDrawingState> stateStack;
    std::vector<CanvasPathSegment> currentPath;
    std::vector<CanvasDrawCommand> commands;

    void Reset(unsigned newWidth, unsigned newHeight) {
        width = newWidth;
        height = newHeight;
        state = {};
        stateStack.clear();
        currentPath.clear();
        commands.clear();
    }
};

} // namespace TWebFrame::Internal
