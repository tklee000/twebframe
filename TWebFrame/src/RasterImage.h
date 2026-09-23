#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

struct RasterImageFrame {
    std::vector<unsigned char> pixels;
    std::uint32_t delayMs = 0;
};

// Decoded pixels are always 32-bit premultiplied BGRA.  Keeping the CPU copy
// makes the resource independent of a particular Direct2D render target and
// lets a target be recreated after a DPI/device change without decoding again.
struct RasterImage {
    std::wstring source;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<RasterImageFrame> frames;
    std::size_t frameIndex = 0;
    // GIF repetitions after the first. UINT32_MAX is the Netscape infinite
    // loop value; zero means a multi-frame GIF without a loop extension plays once.
    std::uint32_t repeatCount = 0;
    std::uint32_t completedLoops = 0;
    std::uint64_t nextFrameTick = 0;
    bool animated = false;
    bool finished = false;

    bool Advance(std::uint64_t now);
    std::uint32_t TimeUntilNextFrame(std::uint64_t now) const;
};

std::shared_ptr<RasterImage> DecodeRasterImage(
    const std::vector<unsigned char>& bytes,
    const std::wstring& source,
    std::wstring* error = nullptr);

bool DecodeImageDataUrl(const std::wstring& source,
                        std::vector<unsigned char>& bytes);

} // namespace TWebFrame::Internal
