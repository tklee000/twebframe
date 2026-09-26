#include <TWebFrame/TWebFrame.h>
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

std::wstring Read(const wchar_t* path) {
    std::ifstream input{std::filesystem::path(path),std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    const int length=MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    std::wstring result(length,L'\0');
    MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),result.data(),length);
    return result;
}
template<class F> void Time(const wchar_t* label,F&& action) {
    const auto start=std::chrono::steady_clock::now();action();
    std::wcout<<label<<L": "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<L" ms"<<std::endl;
}
int wmain(int argc,wchar_t** argv) {
    if(argc<3)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    auto host=CreateWindowExW(0,L"STATIC",L"Content benchmark",WS_OVERLAPPEDWINDOW,0,0,1600,1000,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    auto view=TWebFrame::View::Create(host,RECT{0,0,1500,900});
    bool ok=true;std::wstring result,error;
    Time(L"navigate",[&]{ok=view->Navigate(argv[1]);});
    for(int i=2;i<argc&&ok;++i){
        const auto content=Read(argv[i]);
        Time(argv[i],[&]{
            if(std::filesystem::path(argv[i]).extension()==L".js")ok=view->ExecuteScript(content,&result,&error);
            else ok=view->PostWebMessageAsJson(content,&error);
        });
        if(!ok)std::wcerr<<error<<std::endl;
        Time(L"layout",[&]{view->ExecuteScript(L"return document.body.getBoundingClientRect().width;",&result,&error);});
        Time(L"paint",[&]{SendMessageW(view->Window(),WM_PAINT,0,0);});
    }
    view.reset();DestroyWindow(host);CoUninitialize();return ok?0:1;
}
