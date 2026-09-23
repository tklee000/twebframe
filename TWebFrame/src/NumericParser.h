#pragma once

#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>

namespace TWebFrame::Internal {

inline bool TryParseDouble(const std::wstring& text,double& result,
                           size_t* consumed=nullptr) noexcept {
    if(text.empty())return false;
    wchar_t* end=nullptr;errno=0;
    const double parsed=std::wcstod(text.c_str(),&end);
    if(end==text.c_str()||errno==ERANGE)return false;
    result=parsed;
    if(consumed)*consumed=static_cast<size_t>(end-text.c_str());
    return true;
}

inline bool TryParseFloat(const std::wstring& text,float& result,
                          size_t* consumed=nullptr) noexcept {
    if(text.empty())return false;
    wchar_t* end=nullptr;errno=0;
    const float parsed=std::wcstof(text.c_str(),&end);
    if(end==text.c_str()||errno==ERANGE)return false;
    result=parsed;
    if(consumed)*consumed=static_cast<size_t>(end-text.c_str());
    return true;
}

inline bool TryParseInteger(const std::wstring& text,int& result,
                            size_t* consumed=nullptr,int base=10) noexcept {
    if(text.empty())return false;
    wchar_t* end=nullptr;errno=0;
    const long long parsed=std::wcstoll(text.c_str(),&end,base);
    if(end==text.c_str()||errno==ERANGE||
       parsed<(std::numeric_limits<int>::min)()||
       parsed>(std::numeric_limits<int>::max)())return false;
    result=static_cast<int>(parsed);
    if(consumed)*consumed=static_cast<size_t>(end-text.c_str());
    return true;
}

inline bool TryParseUnsignedInteger(const std::wstring& text,unsigned long long& result,
                                    size_t* consumed=nullptr,int base=10) noexcept {
    if(text.empty())return false;
    size_t first=0;while(first<text.size()&&std::iswspace(text[first]))++first;
    if(first<text.size()&&text[first]==L'-')return false;
    wchar_t* end=nullptr;errno=0;
    const unsigned long long parsed=std::wcstoull(text.c_str(),&end,base);
    if(end==text.c_str()||errno==ERANGE)return false;
    result=parsed;
    if(consumed)*consumed=static_cast<size_t>(end-text.c_str());
    return true;
}

}
