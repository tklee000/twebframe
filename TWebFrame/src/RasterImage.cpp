#include "RasterImage.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace TWebFrame::Internal {
using Microsoft::WRL::ComPtr;

namespace {

class ScopedComInitialization {
public:
    ScopedComInitialization() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedComInitialization() {
        if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
    }
    bool Available() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT result_;
};

bool MetadataValue(IWICMetadataQueryReader* reader, const wchar_t* path,
                   PROPVARIANT& value) {
    PropVariantInit(&value);
    return reader && SUCCEEDED(reader->GetMetadataByName(path, &value));
}

std::uint32_t VariantUnsigned(const PROPVARIANT& value, std::uint32_t fallback = 0) {
    switch (value.vt) {
    case VT_UI1: return value.bVal;
    case VT_UI2: return value.uiVal;
    case VT_UI4: return value.ulVal;
    case VT_I1: return value.cVal < 0 ? fallback : static_cast<std::uint32_t>(value.cVal);
    case VT_I2: return value.iVal < 0 ? fallback : static_cast<std::uint32_t>(value.iVal);
    case VT_I4: return value.lVal < 0 ? fallback : static_cast<std::uint32_t>(value.lVal);
    default: return fallback;
    }
}

std::uint32_t MetadataUnsigned(IWICMetadataQueryReader* reader,
                               const wchar_t* path,
                               std::uint32_t fallback = 0) {
    PROPVARIANT value{};
    if (!MetadataValue(reader, path, value)) return fallback;
    const auto result = VariantUnsigned(value, fallback);
    PropVariantClear(&value);
    return result;
}

bool MetadataBool(IWICMetadataQueryReader* reader, const wchar_t* path) {
    PROPVARIANT value{};
    if (!MetadataValue(reader, path, value)) return false;
    const bool result = value.vt == VT_BOOL ? value.boolVal != VARIANT_FALSE :
                        VariantUnsigned(value) != 0;
    PropVariantClear(&value);
    return result;
}

bool GifRepeatCount(IWICMetadataQueryReader* reader,std::uint32_t& result) {
    PROPVARIANT application{};
    if (!MetadataValue(reader, L"/appext/application", application)) return false;
    bool netscape = false;
    if (application.vt == (VT_UI1 | VT_VECTOR) && application.caub.pElems &&
        application.caub.cElems >= 11) {
        const char signature[] = "NETSCAPE2.0";
        netscape = std::memcmp(application.caub.pElems, signature, 11) == 0;
    }
    PropVariantClear(&application);
    if (!netscape) return false;

    PROPVARIANT data{};
    if (!MetadataValue(reader, L"/appext/data", data)) return false;
    bool found=false;result=0;
    if (data.vt == (VT_UI1 | VT_VECTOR) && data.caub.pElems &&
        data.caub.cElems >= 4 && data.caub.pElems[0] >= 3 &&
        data.caub.pElems[1] == 1) {
        result = static_cast<std::uint32_t>(data.caub.pElems[2]) |
                 (static_cast<std::uint32_t>(data.caub.pElems[3]) << 8);
        found=true;
    }
    PropVariantClear(&data);
    return found;
}

std::uint32_t GifBackground(IWICImagingFactory* factory,
                            IWICBitmapDecoder* decoder,
                            IWICMetadataQueryReader* reader) {
    const auto index = MetadataUnsigned(reader, L"/logscrdesc/BackgroundColorIndex",
                                         std::numeric_limits<std::uint32_t>::max());
    if (index == std::numeric_limits<std::uint32_t>::max()) return 0;
    ComPtr<IWICPalette> palette;
    if (!factory || FAILED(factory->CreatePalette(&palette)) ||
        FAILED(decoder->CopyPalette(palette.Get()))) return 0;
    UINT count = 0;
    if (FAILED(palette->GetColorCount(&count)) || index >= count) return 0;
    std::vector<WICColor> colors(count);
    UINT actual = 0;
    if (FAILED(palette->GetColors(count, colors.data(), &actual)) || index >= actual) return 0;
    const WICColor argb = colors[index];
    const unsigned a = (argb >> 24) & 0xff;
    const unsigned r = (argb >> 16) & 0xff;
    const unsigned g = (argb >> 8) & 0xff;
    const unsigned b = argb & 0xff;
    return (a << 24) | ((r * a + 127) / 255 << 16) |
           ((g * a + 127) / 255 << 8) | ((b * a + 127) / 255);
}

void FillRect(std::vector<unsigned char>& canvas, std::uint32_t canvasWidth,
              std::uint32_t canvasHeight, std::uint32_t left, std::uint32_t top,
              std::uint32_t width, std::uint32_t height, std::uint32_t pbgra) {
    if(left>=canvasWidth||top>=canvasHeight)return;
    const auto right = left + std::min(width, canvasWidth - left);
    const auto bottom = top + std::min(height, canvasHeight - top);
    for (auto y = top; y < bottom; ++y) {
        auto* row = canvas.data() + (static_cast<std::size_t>(y) * canvasWidth + left) * 4;
        for (auto x = left; x < right; ++x, row += 4) {
            row[0] = static_cast<unsigned char>(pbgra & 0xff);
            row[1] = static_cast<unsigned char>((pbgra >> 8) & 0xff);
            row[2] = static_cast<unsigned char>((pbgra >> 16) & 0xff);
            row[3] = static_cast<unsigned char>((pbgra >> 24) & 0xff);
        }
    }
}

void CompositeFrame(std::vector<unsigned char>& canvas, std::uint32_t canvasWidth,
                    std::uint32_t canvasHeight,
                    const std::vector<unsigned char>& source,
                    std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                    std::uint32_t left, std::uint32_t top) {
    const auto width = std::min(sourceWidth, left < canvasWidth ? canvasWidth - left : 0u);
    const auto height = std::min(sourceHeight, top < canvasHeight ? canvasHeight - top : 0u);
    for (std::uint32_t y = 0; y < height; ++y) {
        auto* destination = canvas.data() +
            (static_cast<std::size_t>(top + y) * canvasWidth + left) * 4;
        const auto* pixel = source.data() + static_cast<std::size_t>(y) * sourceWidth * 4;
        for (std::uint32_t x = 0; x < width; ++x, destination += 4, pixel += 4) {
            const unsigned alpha = pixel[3];
            if (!alpha) continue;
            const unsigned inverse = 255 - alpha;
            destination[0] = static_cast<unsigned char>(pixel[0] +
                (destination[0] * inverse + 127) / 255);
            destination[1] = static_cast<unsigned char>(pixel[1] +
                (destination[1] * inverse + 127) / 255);
            destination[2] = static_cast<unsigned char>(pixel[2] +
                (destination[2] * inverse + 127) / 255);
            destination[3] = static_cast<unsigned char>(alpha +
                (destination[3] * inverse + 127) / 255);
        }
    }
}

bool ConvertFrame(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                  std::vector<unsigned char>& pixels,
                  std::uint32_t& width, std::uint32_t& height) {
    if(!factory||!frame)return false;
    ComPtr<IWICBitmapSource> transformed=frame;
    ComPtr<IWICMetadataQueryReader> metadata;frame->GetMetadataQueryReader(&metadata);
    const auto orientation=MetadataUnsigned(metadata.Get(),L"/app1/ifd/{ushort=274}",1);
    WICBitmapTransformOptions transform=WICBitmapTransformRotate0;
    switch(orientation){
    case 2:transform=WICBitmapTransformFlipHorizontal;break;
    case 3:transform=WICBitmapTransformRotate180;break;
    case 4:transform=WICBitmapTransformFlipVertical;break;
    case 5:transform=static_cast<WICBitmapTransformOptions>(
        WICBitmapTransformRotate90|WICBitmapTransformFlipHorizontal);break;
    case 6:transform=WICBitmapTransformRotate90;break;
    case 7:transform=static_cast<WICBitmapTransformOptions>(
        WICBitmapTransformRotate270|WICBitmapTransformFlipHorizontal);break;
    case 8:transform=WICBitmapTransformRotate270;break;
    default:break;
    }
    ComPtr<IWICBitmapFlipRotator> rotator;
    if(transform!=WICBitmapTransformRotate0&&
       SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator))&&
       SUCCEEDED(rotator->Initialize(frame,transform)))transformed=rotator;
    UINT frameWidth = 0, frameHeight = 0;
    if (FAILED(transformed->GetSize(&frameWidth, &frameHeight)) ||
        !frameWidth || !frameHeight) return false;
    const auto byteCount=static_cast<std::uint64_t>(frameWidth)*frameHeight*4;
    if (frameWidth>std::numeric_limits<UINT>::max()/4||
        byteCount>std::numeric_limits<UINT>::max()||
        byteCount>std::numeric_limits<std::size_t>::max())return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(transformed.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    const UINT stride = frameWidth * 4;
    pixels.resize(static_cast<std::size_t>(byteCount));
    if (FAILED(converter->CopyPixels(nullptr, stride,
            static_cast<UINT>(pixels.size()), pixels.data()))) return false;
    width = frameWidth;
    height = frameHeight;
    return true;
}

int Base64Value(wchar_t value) {
    if (value >= L'A' && value <= L'Z') return value - L'A';
    if (value >= L'a' && value <= L'z') return value - L'a' + 26;
    if (value >= L'0' && value <= L'9') return value - L'0' + 52;
    if (value == L'+') return 62;
    if (value == L'/') return 63;
    return -1;
}

int HexValue(wchar_t value) {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

} // namespace

bool RasterImage::Advance(std::uint64_t now) {
    if (!animated || finished || frames.size() < 2) return false;
    if (!nextFrameTick) nextFrameTick = now + std::max(1u, frames[frameIndex].delayMs);
    bool changed = false;
    // Catch up after a blocked UI thread, while bounding malformed zero-delay
    // animations so they cannot monopolize the message loop.
    for (std::size_t count = 0; count < frames.size() * 2 && now >= nextFrameTick; ++count) {
        auto next = frameIndex + 1;
        if (next >= frames.size()) {
            ++completedLoops;
            if (repeatCount != std::numeric_limits<std::uint32_t>::max() &&
                completedLoops > repeatCount) {
                finished = true;
                frameIndex = frames.size() - 1;
                return changed;
            }
            next = 0;
        }
        frameIndex = next;
        changed = true;
        nextFrameTick += std::max(1u, frames[frameIndex].delayMs);
    }
    if (now >= nextFrameTick)
        nextFrameTick = now + std::max(1u, frames[frameIndex].delayMs);
    return changed;
}

std::uint32_t RasterImage::TimeUntilNextFrame(std::uint64_t now) const {
    if (!animated || finished || frames.size() < 2) return 0;
    if (!nextFrameTick || nextFrameTick <= now) return 1;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        nextFrameTick - now, std::numeric_limits<std::uint32_t>::max()));
}

std::shared_ptr<RasterImage> DecodeRasterImage(
    const std::vector<unsigned char>& bytes, const std::wstring& source,
    std::wstring* error) {
    if (error) error->clear();
    if (bytes.empty()) {
        if (error) *error = L"Image resource is empty";
        return {};
    }
    ScopedComInitialization com;
    if (!com.Available()) {
        if (error) *error = L"COM initialization failed";
        return {};
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) return {};
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return {}; }
    std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return {};
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder))) {
        if (error) *error = L"Unsupported or invalid JPEG, PNG, or GIF image";
        return {};
    }

    GUID container{};
    decoder->GetContainerFormat(&container);
    if (container != GUID_ContainerFormatJpeg &&
        container != GUID_ContainerFormatPng &&
        container != GUID_ContainerFormatGif) {
        if (error) *error = L"Only JPEG, PNG, and GIF images are supported";
        return {};
    }

    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || !frameCount) return {};
    auto result = std::make_shared<RasterImage>();
    result->source = source;

    ComPtr<IWICMetadataQueryReader> decoderMetadata;
    decoder->GetMetadataQueryReader(&decoderMetadata);
    const bool gif = container == GUID_ContainerFormatGif;
    if (gif) {
        result->width = MetadataUnsigned(decoderMetadata.Get(), L"/logscrdesc/Width");
        result->height = MetadataUnsigned(decoderMetadata.Get(), L"/logscrdesc/Height");
        std::uint32_t repetitions=0;
        if(GifRepeatCount(decoderMetadata.Get(),repetitions))
            result->repeatCount=repetitions?repetitions:
                std::numeric_limits<std::uint32_t>::max();
    }

    std::vector<unsigned char> canvas;
    std::vector<unsigned char> restorePrevious;
    std::uint32_t previousLeft = 0, previousTop = 0, previousWidth = 0, previousHeight = 0;
    std::uint32_t previousDisposal = 0, previousClear = 0;
    for (UINT index = 0; index < frameCount; ++index) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(index, &frame))) return {};
        std::vector<unsigned char> framePixels;
        std::uint32_t frameWidth = 0, frameHeight = 0;
        if (!ConvertFrame(factory.Get(), frame.Get(), framePixels, frameWidth, frameHeight)) return {};

        if (!gif) {
            result->width = frameWidth;
            result->height = frameHeight;
            result->frames.push_back({std::move(framePixels), 0});
            break;
        }
        if (!result->width) result->width = frameWidth;
        if (!result->height) result->height = frameHeight;
        if (static_cast<std::uint64_t>(result->width) * result->height >
            std::numeric_limits<std::size_t>::max() / 4) return {};
        if (canvas.empty()) canvas.assign(
            static_cast<std::size_t>(result->width) * result->height * 4, 0);

        if (index) {
            if (previousDisposal == 2)
                FillRect(canvas, result->width, result->height, previousLeft, previousTop,
                         previousWidth, previousHeight, previousClear);
            else if (previousDisposal == 3 && restorePrevious.size() == canvas.size())
                canvas = restorePrevious;
        }

        ComPtr<IWICMetadataQueryReader> metadata;
        frame->GetMetadataQueryReader(&metadata);
        const auto left = MetadataUnsigned(metadata.Get(), L"/imgdesc/Left");
        const auto top = MetadataUnsigned(metadata.Get(), L"/imgdesc/Top");
        const auto disposal = MetadataUnsigned(metadata.Get(), L"/grctlext/Disposal");
        const auto delay = MetadataUnsigned(metadata.Get(), L"/grctlext/Delay");
        const bool transparent = MetadataBool(metadata.Get(), L"/grctlext/TransparencyFlag");
        if (disposal == 3) restorePrevious = canvas;
        CompositeFrame(canvas, result->width, result->height, framePixels,
                       frameWidth, frameHeight, left, top);
        // Browsers protect the event loop from zero/one-centisecond GIFs by
        // using a 100 ms fallback, while preserving authored delays otherwise.
        const std::uint32_t delayMs = delay <= 1 ? 100u : delay * 10u;
        result->frames.push_back({canvas, delayMs});
        previousLeft = left; previousTop = top;
        previousWidth = frameWidth; previousHeight = frameHeight;
        previousDisposal = disposal;
        previousClear = transparent ? 0u :
            GifBackground(factory.Get(), decoder.Get(), decoderMetadata.Get());
    }
    result->animated = result->frames.size() > 1;
    if (!result->width || !result->height || result->frames.empty()) return {};
    return result;
}

bool DecodeImageDataUrl(const std::wstring& source,
                        std::vector<unsigned char>& bytes) {
    bytes.clear();
    if (source.size() < 6 || source.compare(0, 5, L"data:") != 0) return false;
    const auto comma = source.find(L',');
    if (comma == std::wstring::npos) return false;
    auto metadata = source.substr(5, comma - 5);
    std::transform(metadata.begin(), metadata.end(), metadata.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (metadata.rfind(L"image/jpeg", 0) != 0 &&
        metadata.rfind(L"image/jpg", 0) != 0 &&
        metadata.rfind(L"image/png", 0) != 0 &&
        metadata.rfind(L"image/gif", 0) != 0) return false;
    const auto payload = source.substr(comma + 1);
    if (metadata.find(L";base64") != std::wstring::npos) {
        int accumulator = 0, bits = -8;
        for (const auto character : payload) {
            if (character == L'=' || std::iswspace(character)) continue;
            const int value = Base64Value(character);
            if (value < 0) return false;
            accumulator = (accumulator << 6) | value;
            bits += 6;
            if (bits >= 0) {
                bytes.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
                bits -= 8;
            }
        }
        return !bytes.empty();
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        if (payload[index] == L'%' && index + 2 < payload.size()) {
            const int high = HexValue(payload[index + 1]);
            const int low = HexValue(payload[index + 2]);
            if (high < 0 || low < 0) return false;
            bytes.push_back(static_cast<unsigned char>((high << 4) | low));
            index += 2;
        } else if (payload[index] <= 0xff) {
            bytes.push_back(static_cast<unsigned char>(payload[index]));
        } else return false;
    }
    return !bytes.empty();
}

} // namespace TWebFrame::Internal
