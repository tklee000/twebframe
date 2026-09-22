#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;

namespace {

int failures=0;

void Check(bool condition,const wchar_t* message){
    if(!condition){std::wcerr<<L"FAIL: "<<message<<L'\n';++failures;}
}

bool Near(BYTE actual,BYTE expected,BYTE tolerance=12){
    return std::abs(static_cast<int>(actual)-static_cast<int>(expected))<=tolerance;
}

struct Pixel { BYTE blue=0,green=0,red=0,alpha=0; };

struct Raster {
    UINT width=0,height=0,stride=0;
    std::vector<BYTE> bytes;

    Pixel At(float dipX,float dipY,float scale) const {
        const auto x=std::min(width-1,static_cast<UINT>(std::lround(dipX*scale)));
        const auto y=std::min(height-1,static_cast<UINT>(std::lround(dipY*scale)));
        const auto* value=bytes.data()+y*stride+x*4;
        return {value[0],value[1],value[2],value[3]};
    }
};

Raster Paint(LayoutEngine& layout,float scale){
    Raster result;
    result.width=static_cast<UINT>(std::lround(40.0f*scale));
    result.height=static_cast<UINT>(std::lround(24.0f*scale));
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> writeFactory;
    const bool resources=
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory)))&&
        SUCCEEDED(wicFactory->CreateBitmap(result.width,result.height,
            GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2dFactory.ReleaseAndGetAddressOf()))&&
        SUCCEEDED(d2dFactory->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
                USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale),&target))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())));
    Check(resources,L"form-control raster resources are created");
    if(!resources)return result;

    target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    layout.Paint(target.Get(),writeFactory.Get());
    Check(SUCCEEDED(target->EndDraw()),L"form-control fixture paints");

    WICRect rectangle{0,0,static_cast<INT>(result.width),static_cast<INT>(result.height)};
    ComPtr<IWICBitmapLock> lock;
    Check(SUCCEEDED(bitmap->Lock(&rectangle,WICBitmapLockRead,&lock)),
          L"form-control pixels are readable");
    if(!lock)return result;
    UINT size=0;BYTE* bytes=nullptr;
    Check(SUCCEEDED(lock->GetStride(&result.stride))&&
          SUCCEEDED(lock->GetDataPointer(&size,&bytes))&&bytes,
          L"form-control pixel buffer is available");
    if(bytes)result.bytes.assign(bytes,bytes+size);
    return result;
}

bool IsWhite(const Pixel& value){
    return Near(value.red,255)&&Near(value.green,255)&&Near(value.blue,255)&&value.alpha>240;
}

bool IsBlue(const Pixel& value){
    return Near(value.red,13)&&Near(value.green,110)&&Near(value.blue,253)&&value.alpha>240;
}

bool IsGray(const Pixel& value){
    return Near(value.red,191,18)&&Near(value.green,191,18)&&Near(value.blue,191,18)&&value.alpha>240;
}

void CheckScale(float scale){
    const wchar_t* html=LR"HTML(
        <style>
            :root { --body-bg:#fff; }
            html,body { margin:0; padding:0; background:#000; }
            .switch {
                box-sizing:border-box; display:block; width:32px; height:16px; margin:4px;
                -webkit-appearance:none; appearance:none;
                --form-control-bg:var(--body-bg);
                background-color:var(--form-control-bg);
                background-image:var(--switch-image);
                background-repeat:no-repeat; background-position:left center;
                background-size:contain; border:1px solid #dee2e6; border-radius:32px;
                --switch-image:url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='-4 -4 8 8'%3e%3ccircle r='3' fill='rgba%280, 0, 0, 0.25%29'/%3e%3c/svg%3e");
            }
            .switch:checked {
                background-color:#0d6efd; border-color:#0d6efd;
                background-position:right center;
                --switch-image:url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='-4 -4 8 8'%3e%3ccircle r='3' fill='%23fff'/%3e%3c/svg%3e");
            }
            .switch[type=checkbox]:indeterminate { background-color:#0d6efd; }
            .switch:unsupported-state { background-color:#dc3545; }
        </style>
        <input class="switch" type="checkbox" checked>
    )HTML";

    std::wstring error;
    Document document;Check(document.Parse(html,&error),error.c_str());
    StyleSheet styles;Check(styles.Parse(document.StyleText(),&error),error.c_str());
    const auto control=document.QuerySelector(L".switch");
    Check(control&&control->checked,L"checked attribute initializes the shared DOM checked state");
    if(!control)return;

    LayoutEngine layout(document,styles);layout.Layout(40.0f,24.0f,scale);
    const auto* checkedBox=layout.BoxFor(control);
    Check(checkedBox&&checkedBox->style.Get(L"appearance")==L"none",
          L"appearance none reaches the common form-control painter");
    Check(checkedBox&&checkedBox->style.Get(L"background-color")==L"#0d6efd"&&
          checkedBox->style.Get(L"background-position")==L"right center",
          L"checked pseudo-class resolves the checked switch paint rules");
    if(checkedBox){
        Check(std::abs(checkedBox->rect.width-32.0f)<0.01f&&
              std::abs(checkedBox->rect.height-16.0f)<0.01f,
              L"switch layout remains CSS-pixel based at the active DPI");
    }
    auto raster=Paint(layout,scale);
    if(!raster.bytes.empty()){
        Check(IsBlue(raster.At(10.0f,12.0f,scale)),
              L"checked switch leaves the leading side blue");
        Check(IsWhite(raster.At(26.0f,12.0f,scale)),
              L"checked switch paints its SVG thumb on the trailing side");
    }

    JavaScriptRuntime javascript(document);
    Check(javascript.Execute(
        L"const control=document.querySelector('.switch');control.checked=false;"
        L"control.indeterminate=true;return control.indeterminate;",
        nullptr,&error),error.c_str());
    Check(!control->checked,L"the shared JavaScript checked setter updates DOM state");
    Check(control->indeterminate,L"the shared JavaScript indeterminate setter updates DOM state");
    layout.Layout(40.0f,24.0f,scale);
    const auto* mixedBox=layout.BoxFor(control);
    Check(mixedBox&&mixedBox->style.Get(L"background-color")==L"#0d6efd",
          L"indeterminate pseudo-class follows the shared DOM state");

    Check(javascript.Execute(
        L"document.querySelector('.switch').indeterminate=false;",
        nullptr,&error),error.c_str());
    Check(!control->indeterminate,L"JavaScript can restore an ordinary unchecked state");
    layout.Layout(40.0f,24.0f,scale);
    const auto* uncheckedBox=layout.BoxFor(control);
    Check(uncheckedBox&&uncheckedBox->style.Get(L"background-color")==L"#fff"&&
          uncheckedBox->style.Get(L"background-position")==L"left center",
          L"unchecked pseudo-class restores the base switch paint rules");
    raster=Paint(layout,scale);
    if(!raster.bytes.empty()){
        Check(IsGray(raster.At(10.0f,12.0f,scale)),
              L"unchecked switch paints its SVG thumb on the leading side");
        Check(IsWhite(raster.At(26.0f,12.0f,scale)),
              L"unchecked switch restores the light track on the trailing side");
    }
}

void CheckNativeCheckboxGeometry(){
    std::wstring error;Document document;
    Check(document.Parse(
        L"<style>html,body{margin:0}*{box-sizing:border-box}.check{width:100px;height:25px;display:flex;"
        L"align-items:center;gap:7px}.check input{width:15px;height:15px}</style>"
        L"<label class='check'><input class='native' type='checkbox'><span>Choice</span></label>",
        &error),error.c_str());
    StyleSheet styles;Check(styles.Parse(document.StyleText(),&error),error.c_str());
    LayoutEngine layout(document,styles);layout.Layout(200,60);
    const auto control=document.QuerySelector(L".native");
    const auto label=document.QuerySelector(L".native + span");
    const auto* controlBox=layout.BoxFor(control);const auto* labelBox=layout.BoxFor(label);
    Check(controlBox&&labelBox&&std::lround(controlBox->rect.x)==4&&
          std::lround(labelBox->rect.x)==29,
          L"native checkbox margins and flex gap retain browser geometry");
}

} // namespace

int wmain(){
    const auto initialized=SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));
    CheckScale(1.0f);
    CheckScale(1.5f);
    CheckNativeCheckboxGeometry();
    if(initialized)CoUninitialize();
    if(failures){std::wcerr<<failures<<L" form-control regression check(s) failed\n";return 1;}
    std::wcout<<L"Form-control regression checks passed\n";
    return 0;
}
