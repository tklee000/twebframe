#include "JavaScript.h"
#include "Canvas.h"
#include "NumericParser.h"
#include "RasterImage.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace TWebFrame::Internal {
namespace {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if(value.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring result(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),count);
    return result;
}

int HexDigitValue(wchar_t character) {
    if(character>=L'0'&&character<=L'9')return character-L'0';
    if(character>=L'a'&&character<=L'f')return character-L'a'+10;
    if(character>=L'A'&&character<=L'F')return character-L'A'+10;
    return -1;
}

bool TryParseDecimalIndex(const std::wstring& text,size_t& result) {
    if(text.empty())return false;
    size_t value=0;
    constexpr auto maximum=(std::numeric_limits<size_t>::max)();
    for(const auto character:text){
        if(character<L'0'||character>L'9')return false;
        const auto digit=static_cast<size_t>(character-L'0');
        if(value>(maximum-digit)/10)return false;
        value=value*10+digit;
    }
    result=value;return true;
}

void AppendCodePoint(std::wstring& output,std::uint32_t codePoint) {
    if(codePoint>0x10ffff||(codePoint>=0xd800&&codePoint<=0xdfff))codePoint=0xfffd;
    if constexpr(sizeof(wchar_t)>=4)output+=static_cast<wchar_t>(codePoint);
    else if(codePoint<=0xffff)output+=static_cast<wchar_t>(codePoint);
    else{
        codePoint-=0x10000;
        output+=static_cast<wchar_t>(0xd800+(codePoint>>10));
        output+=static_cast<wchar_t>(0xdc00+(codePoint&0x3ff));
    }
}

bool ParseUnicodeEscape(const std::wstring& source,size_t& position,std::uint32_t& codePoint) {
    if(position>=source.size()||source[position]!=L'u')return false;
    size_t cursor=position+1;std::uint32_t value=0;
    if(cursor<source.size()&&source[cursor]==L'{'){
        ++cursor;const size_t digits=cursor;
        while(cursor<source.size()&&source[cursor]!=L'}'){
            const int digit=HexDigitValue(source[cursor]);
            if(digit<0||cursor-digits>=6)return false;
            value=value*16+static_cast<std::uint32_t>(digit);++cursor;
        }
        if(cursor==digits||cursor>=source.size()||source[cursor]!=L'}'||value>0x10ffff)return false;
        ++cursor;
    }else{
        if(cursor+4>source.size())return false;
        for(size_t index=0;index<4;++index){
            const int digit=HexDigitValue(source[cursor+index]);if(digit<0)return false;
            value=value*16+static_cast<std::uint32_t>(digit);
        }
        cursor+=4;
    }
    codePoint=value;position=cursor;return true;
}

std::wstring DecodeRegexUnicodeEscapes(const std::wstring& pattern) {
    std::wstring output;output.reserve(pattern.size());
    for(size_t position=0;position<pattern.size();){
        if(pattern[position]==L'\\'&&position+1<pattern.size()&&pattern[position+1]==L'u'){
            size_t end=position+1;std::uint32_t codePoint=0;
            if(ParseUnicodeEscape(pattern,end,codePoint)){
                std::wstring decoded;AppendCodePoint(decoded,codePoint);
                for(const auto character:decoded){
                    if(std::wstring_view(L"\\.^$|()[]{}*+?").find(character)!=std::wstring_view::npos)
                        output+=L'\\';
                    output+=character;
                }
                position=end;continue;
            }
        }
        output+=pattern[position++];
    }
    return output;
}

void AppendRegexClassCharacter(std::wstring& output,wchar_t character) {
    if(std::wstring_view(L"\\]-^").find(character)!=std::wstring_view::npos)
        output+=L'\\';
    output+=character;
}

const std::wstring& UnicodeLetterClassRanges() {
    static const std::wstring ranges=[](){
        std::wstring result;
        std::wstring characters(0x10000,L'\0');
        for(unsigned value=0;value<characters.size();++value)
            characters[value]=static_cast<wchar_t>(value);
        std::vector<WORD> types(characters.size());
        if(!GetStringTypeW(CT_CTYPE1,characters.data(),static_cast<int>(characters.size()),
                           types.data()))return result;
        const auto isLetter=[&](unsigned candidate){
            return !(candidate>=0xd800&&candidate<=0xdfff)&&
                   (types[candidate]&C1_ALPHA)!=0;
        };
        for(unsigned value=0;value<=0xffff;){
            if(!isLetter(value)){++value;continue;}
            const unsigned first=value;
            while(value<0xffff&&isLetter(value+1))++value;
            AppendRegexClassCharacter(result,static_cast<wchar_t>(first));
            if(value!=first){
                result+=L'-';
                AppendRegexClassCharacter(result,static_cast<wchar_t>(value));
            }
            ++value;
        }
        return result;
    }();
    return ranges;
}

bool TranslateUnicodePropertyEscapes(const std::wstring& pattern,const std::wstring& flags,
                                     std::wstring& output) {
    output.clear();output.reserve(pattern.size());
    const bool unicode=flags.find(L'u')!=std::wstring::npos||flags.find(L'v')!=std::wstring::npos;
    bool characterClass=false;
    for(size_t position=0;position<pattern.size();++position){
        const wchar_t character=pattern[position];
        if(character==L'\\'&&position+3<pattern.size()&&
           (pattern[position+1]==L'p'||pattern[position+1]==L'P')&&
           pattern[position+2]==L'{'){
            const auto end=pattern.find(L'}',position+3);
            if(!unicode||end==std::wstring::npos)return false;
            const auto property=pattern.substr(position+3,end-(position+3));
            if(property!=L"L"&&property!=L"Letter"&&
               property!=L"General_Category=L"&&property!=L"General_Category=Letter"&&
               property!=L"gc=L"&&property!=L"gc=Letter")return false;
            const bool negated=pattern[position+1]==L'P';
            if(characterClass&&negated)return false;
            if(!characterClass)output+=negated?L"[^":L"[";
            output+=UnicodeLetterClassRanges();
            if(!characterClass)output+=L']';
            position=end;continue;
        }
        output+=character;
        if(character==L'\\'&&position+1<pattern.size())output+=pattern[++position];
        else if(character==L'['&&!characterClass)characterClass=true;
        else if(character==L']'&&characterClass)characterClass=false;
    }
    return true;
}

// A large share of split separators are finite regular expressions made from
// literal characters and greedy optional characters (for example /--?/ or
// /\r?\n/).  Running the general MSVC regex engine once per match is very
// expensive on multi-megabyte strings, so compile that whole regex class into
// ordered literal alternatives.  Unsupported expressions still use wregex.
struct FiniteRegexSeparator {
    std::vector<std::wstring> alternatives;
    bool ignoreCase=false;

    bool MatchAt(const std::wstring& input,size_t position,size_t& length) const {
        for(const auto& alternative:alternatives){
            if(alternative.size()>input.size()-position)continue;
            bool matches=true;
            for(size_t index=0;index<alternative.size();++index){
                auto left=input[position+index],right=alternative[index];
                if(ignoreCase){left=static_cast<wchar_t>(std::towlower(left));right=static_cast<wchar_t>(std::towlower(right));}
                if(left!=right){matches=false;break;}
            }
            if(matches){length=alternative.size();return true;}
        }
        return false;
    }
};

bool CompileFiniteRegexSeparator(const std::wstring& pattern,const std::wstring& flags,
                                 FiniteRegexSeparator& result){
    result.alternatives.assign(1,L"");
    result.ignoreCase=flags.find(L'i')!=std::wstring::npos;
    for(size_t position=0;position<pattern.size();++position){
        wchar_t literal=pattern[position];
        if(literal==L'\\'){
            if(++position>=pattern.size())return false;
            const auto escaped=pattern[position];
            switch(escaped){
            case L'r':literal=L'\r';break;
            case L'n':literal=L'\n';break;
            case L't':literal=L'\t';break;
            case L'f':literal=L'\f';break;
            case L'v':literal=L'\v';break;
            case L'0':literal=L'\0';break;
            default:
                if(std::iswalnum(escaped))return false;
                literal=escaped;break;
            }
        }else if(std::wstring_view(L".^$|?*+()[]{}").find(literal)!=std::wstring_view::npos){
            return false;
        }
        const bool optional=position+1<pattern.size()&&pattern[position+1]==L'?';
        if(optional)++position;
        if(optional){
            if(result.alternatives.size()>64)return false;
            std::vector<std::wstring> expanded;
            expanded.reserve(result.alternatives.size()*2);
            for(const auto& alternative:result.alternatives){
                expanded.push_back(alternative+literal);
                expanded.push_back(alternative);
            }
            result.alternatives=std::move(expanded);
        }else{
            for(auto& alternative:result.alternatives)alternative+=literal;
        }
    }
    return true;
}

struct AnchoredRegexPrefixes {
    std::vector<std::wstring> values;
    bool ignoreCase=false;

    bool MayMatch(const std::wstring& input) const {
        if(values.empty())return true;
        for(const auto& prefix:values){
            if(prefix.size()>input.size())continue;
            bool matches=true;
            for(size_t index=0;index<prefix.size();++index){
                auto left=input[index],right=prefix[index];
                if(ignoreCase){left=static_cast<wchar_t>(std::towlower(left));right=static_cast<wchar_t>(std::towlower(right));}
                if(left!=right){matches=false;break;}
            }
            if(matches)return true;
        }
        return false;
    }
};

AnchoredRegexPrefixes CompileAnchoredRegexPrefixes(const std::wstring& pattern,
                                                    const std::wstring& flags){
    AnchoredRegexPrefixes result;result.ignoreCase=flags.find(L'i')!=std::wstring::npos;
    if(pattern.empty()||pattern.front()!=L'^')return result;
    auto prefix=[&](size_t begin,size_t end){
        std::wstring value;
        for(size_t position=begin;position<end;++position){
            auto character=pattern[position];
            if(character==L'\\'){
                if(++position>=end)return std::wstring{};
                const auto escaped=pattern[position];
                if(std::iswalnum(escaped)){
                    if(escaped==L'r')character=L'\r';
                    else if(escaped==L'n')character=L'\n';
                    else if(escaped==L't')character=L'\t';
                    else return value;
                }else character=escaped;
            }else if(character==L'?'||character==L'*'){
                if(!value.empty())value.pop_back();
                return value;
            }else if(character==L'{')return std::wstring{};
            else if(std::wstring_view(L".^$|+()[]}").find(character)!=std::wstring_view::npos)return value;
            value+=character;
        }
        return value;
    };
    size_t start=1;
    if(start<pattern.size()&&pattern[start]==L'('){
        ++start;if(start+1<pattern.size()&&pattern[start]==L'?'&&pattern[start+1]==L':')start+=2;
        size_t alternative=start;int depth=0;
        for(size_t position=start;position<pattern.size();++position){
            if(pattern[position]==L'\\'){++position;continue;}
            if(pattern[position]==L'(')++depth;
            else if(pattern[position]==L')'){
                if(depth==0){
                    auto value=prefix(alternative,position);if(value.empty()){result.values.clear();return result;}
                    result.values.push_back(std::move(value));return result;
                }
                --depth;
            }else if(pattern[position]==L'|'&&depth==0){
                auto value=prefix(alternative,position);if(value.empty()){result.values.clear();return result;}
                result.values.push_back(std::move(value));alternative=position+1;
            }
        }
        result.values.clear();return result;
    }
    auto value=prefix(start,pattern.size());if(!value.empty())result.values.push_back(std::move(value));
    return result;
}

struct Object;
struct Function;
struct NativeFunction;
struct Reference;
struct Environment;
struct Chunk;
struct Prototype;

struct Value {
    enum class Type { Undefined, Null, Boolean, Number, String, Object, Function, Native, Reference };
    Type type = Type::Undefined;
    bool boolean = false;
    double number = 0.0;
    std::wstring string;
    std::shared_ptr<Object> object;
    std::shared_ptr<Function> function;
    std::shared_ptr<NativeFunction> native;
    std::shared_ptr<Reference> reference;

    static Value Undefined() { return {}; }
    static Value Null() { Value v; v.type = Type::Null; return v; }
    static Value Bool(bool b) { Value v; v.type=Type::Boolean; v.boolean=b; return v; }
    static Value Number(double n) { Value v; v.type=Type::Number; v.number=n; return v; }
    static Value String(std::wstring s) { Value v; v.type=Type::String; v.string=std::move(s); return v; }
    static Value FromObject(const std::shared_ptr<Object>& o) { Value v; v.type=Type::Object; v.object=o; return v; }
    static Value FromFunction(const std::shared_ptr<Function>& f) { Value v; v.type=Type::Function; v.function=f; return v; }
    static Value FromNative(const std::shared_ptr<NativeFunction>& f) { Value v; v.type=Type::Native; v.native=f; return v; }
    static Value FromReference(const std::shared_ptr<Reference>& r) { Value v; v.type=Type::Reference; v.reference=r; return v; }
};

struct JavaScriptException {
    Value value;
};

struct PromiseReaction {
    Value onFulfilled;
    Value onRejected;
    std::shared_ptr<Object> nextPromise;
};

enum class PromiseState { Pending, Fulfilled, Rejected };
enum class ObjectKind { Plain, Array, Map, Set, RegExp, Window, FrameWindow, Document, Node, Range, Selection, TreeWalker, ClassList, Style, Dataset, Event, WebView, Performance, Math, Json, ObjectConstructor, ArrayConstructor, StringConstructor, NumberConstructor, DateConstructor, Date, PromiseConstructor, ErrorConstructor, UrlSearchParams, Storage, MediaQuery, Response, Promise, Error, Location, File, FileReader, Clipboard, DataTransfer, DataTransferItem, CanvasContext2D, CanvasGradient };

struct Object {
    ObjectKind kind = ObjectKind::Plain;
    FastMap<std::wstring, Value> props;
    std::vector<Value> items;
    std::vector<std::pair<Value, Value>> entries;
    std::shared_ptr<Node> node;
    std::shared_ptr<Node> rangeStart;
    std::shared_ptr<Node> rangeEnd;
    size_t rangeStartOffset = 0;
    size_t rangeEndOffset = 0;
    std::shared_ptr<CanvasGradient> canvasGradient;
    std::shared_ptr<Object> prototype;
    PromiseState promiseState = PromiseState::Pending;
    Value promiseResult;
    std::vector<PromiseReaction> promiseReactions;
    bool promiseHandled = false;
    bool promiseUnhandledNotified = false;
};

struct Environment {
    FastMap<std::wstring, Value> values;
    std::shared_ptr<Environment> parent;
    Environment* Find(const std::wstring& name) {
        if (values.count(name)) return this;
        return parent ? parent->Find(name) : nullptr;
    }
};

struct Reference {
    enum class Kind { Variable, Property } kind = Kind::Variable;
    std::shared_ptr<Environment> env;
    std::wstring name;
    Value base;
};

enum class Op {
    Constant, Undefined, Null, TrueValue, FalseValue,
    LoadReference, Declare, GetProperty, GetIndex, Assign,
    NewArray, ArrayPush, ArraySpread, NewObject, ObjectSet, ObjectSpread, EnumerableKeys, MakeFunction,
    Call, CallArray, Construct, ConstructArray, DeleteValue, ThrowValue, Await, LeaveCatch, EndFinally, Pop, Duplicate, PostIncrement, PostDecrement, PreIncrement, PreDecrement,
    Add, Subtract, Multiply, Divide, Modulo, Power,
    BitwiseAnd, BitwiseOr, BitwiseXor, ShiftLeft, ShiftRight, UnsignedShiftRight, InValue,
    Equal, NotEqual, StrictEqual, StrictNotEqual, Less, LessEqual, Greater, GreaterEqual,
    Not, BitwiseNot, Negate, Positive, VoidValue, TypeOf,
    Jump, JumpFalse, JumpFalseKeep, JumpTrueKeep, JumpNotNullishKeep,
    Template, Return
};

struct Instruction {
    Op op = Op::Undefined;
    int argument = 0;
    std::wstring text;
};

struct Chunk {
    struct ExceptionHandler {
        size_t tryStart=0,tryEnd=0;
        size_t catchStart=0,catchEnd=0;
        size_t finallyStart=0,finallyEnd=0;
        size_t end=0;
        std::wstring catchName;
        bool hasCatch=false,hasFinally=false;
    };
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<ExceptionHandler> handlers;
};

struct Prototype {
    struct Binding {
        std::wstring parameter, property, name;
        bool indexed=false;
        std::shared_ptr<Chunk> defaultValue;
    };
    std::vector<std::wstring> parameters;
    std::vector<std::shared_ptr<Chunk>> parameterDefaults;
    std::vector<Binding> bindings;
    std::wstring restParameter;
    Chunk chunk;
    std::wstring name;
    bool lexicalThis = false;
    bool isAsync = false;
};

struct Module {
    std::vector<std::shared_ptr<Prototype>> prototypes;
};

struct Function {
    std::shared_ptr<Prototype> prototype;
    std::shared_ptr<Environment> closure;
};

struct RuntimeCore;
using NativeCallback = std::function<Value(RuntimeCore&, const Value&, const std::vector<Value>&)>;
struct NativeFunction { NativeCallback callback; };

enum class CompletionKind { Return, Jump, Throw };
struct PendingCompletion {
    int handler=-1;
    CompletionKind kind=CompletionKind::Return;
    Value value;
    size_t target=0;
};
struct ExecutionFrame {
    struct CatchScope { int handler=-1;std::shared_ptr<Environment> outer; };
    const Chunk* chunk=nullptr;
    std::shared_ptr<Prototype> prototype;
    std::shared_ptr<Environment> env;
    std::vector<Value> stack;
    std::vector<PendingCompletion> pending;
    std::vector<CatchScope> catchScopes;
    size_t ip=0;
    std::shared_ptr<Object> asyncPromise;
    bool resumeThrow=false;
    Value resumeValue;
    size_t resumeOrigin=0;
};
struct FrameResult {
    bool suspended=false;
    Value value;
};

enum class TokenKind {
    End, Identifier, Number, String, Template, RegExp,
    LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
    Dot, Ellipsis, Comma, Colon, Semicolon, Question, OptionalChain, Nullish, NullishAssign,
    Plus, Minus, PlusPlus, MinusMinus, PlusAssign, MinusAssign,
    Star, StarAssign, Exponent, ExponentAssign, Slash, SlashAssign, Percent, PercentAssign, Bang,
    Assign, Equal, NotEqual, StrictEqual, StrictNotEqual, Less, LessEqual, Greater, GreaterEqual,
    ShiftLeft, ShiftLeftAssign, ShiftRight, ShiftRightAssign, UnsignedShiftRight, UnsignedShiftRightAssign,
    BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, BitNot,
    And, AndAssign, Or, OrAssign, Arrow
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::wstring text;
    double number = 0;
    int line = 1;
    std::wstring extra;
};

class Lexer {
public:
    explicit Lexer(const std::wstring& source) : source_(source) {}
    std::vector<Token> Scan() {
        std::vector<Token> tokens;
        for (;;) {
            SkipTrivia();
            if (position_ >= source_.size()) { tokens.push_back({TokenKind::End,L"",0,line_}); break; }
            const wchar_t c = source_[position_++];
            Token token; token.line = line_;
            if (std::iswalpha(c) || c == L'_' || c == L'$') {
                const size_t start = position_ - 1;
                while (position_ < source_.size() && (std::iswalnum(source_[position_]) || source_[position_] == L'_' || source_[position_] == L'$')) ++position_;
                token.kind=TokenKind::Identifier; token.text=source_.substr(start,position_-start); tokens.push_back(token); continue;
            }
            if (std::iswdigit(c) || (c == L'.' && position_ < source_.size() && std::iswdigit(source_[position_]))) {
                const size_t start=position_-1;
                while(position_<source_.size() && (std::iswalnum(source_[position_]) || source_[position_]==L'_' || source_[position_]==L'.' || source_[position_]==L'+' || source_[position_]==L'-')) {
                    if ((source_[position_]==L'+' || source_[position_]==L'-') && source_[position_-1]!=L'e' && source_[position_-1]!=L'E') break;
                    ++position_;
                }
                token.kind=TokenKind::Number; token.text=source_.substr(start,position_-start);
                auto numeric=token.text;numeric.erase(std::remove(numeric.begin(),numeric.end(),L'_'),numeric.end());
                if(!numeric.empty()&&numeric.back()==L'n')numeric.pop_back();size_t used=0;
                if(numeric.size()>2&&numeric[0]==L'0'&&(numeric[1]==L'x'||numeric[1]==L'X')){
                    unsigned long long parsed=0;
                    if(TryParseUnsignedInteger(numeric,parsed,&used,16)&&used==numeric.size())
                        token.number=static_cast<double>(parsed);
                }else{
                    double parsed=0;if(TryParseDouble(numeric,parsed,&used)&&used==numeric.size())token.number=parsed;
                }
                tokens.push_back(token); continue;
            }
            if (c == L'\'' || c == L'"') { token.kind=TokenKind::String; token.text=ReadString(c); tokens.push_back(token); continue; }
            if (c == L'`') { token.kind=TokenKind::Template; token.text=ReadTemplate(); tokens.push_back(token); continue; }
            auto push=[&](TokenKind k){token.kind=k;token.text=std::wstring(1,c);tokens.push_back(token);};
            switch(c) {
            case L'(':push(TokenKind::LeftParen);break; case L')':push(TokenKind::RightParen);break;
            case L'{':push(TokenKind::LeftBrace);break; case L'}':push(TokenKind::RightBrace);break;
            case L'[':push(TokenKind::LeftBracket);break; case L']':push(TokenKind::RightBracket);break;
            case L'.':
                if(position_+1<source_.size()&&source_[position_]==L'.'&&source_[position_+1]==L'.'){
                    position_+=2;token.kind=TokenKind::Ellipsis;token.text=L"...";tokens.push_back(token);
                }else push(TokenKind::Dot);
                break;
            case L',':push(TokenKind::Comma);break;
            case L':':push(TokenKind::Colon);break; case L';':push(TokenKind::Semicolon);break;
            case L'?':
                if(Match(L'?')){const bool assign=Match(L'=');token.kind=assign?TokenKind::NullishAssign:TokenKind::Nullish;token.text=assign?L"??=":L"??";tokens.push_back(token);}
                else if(Match(L'.')){token.kind=TokenKind::OptionalChain;token.text=L"?.";tokens.push_back(token);}
                else push(TokenKind::Question);
                break;
            case L'+':if(Match(L'+'))push(TokenKind::PlusPlus);else if(Match(L'='))push(TokenKind::PlusAssign);else push(TokenKind::Plus);break;
            case L'-':if(Match(L'-'))push(TokenKind::MinusMinus);else if(Match(L'='))push(TokenKind::MinusAssign);else push(TokenKind::Minus);break;
            case L'*':
                if(Match(L'*')){if(Match(L'='))push(TokenKind::ExponentAssign);else push(TokenKind::Exponent);}
                else if(Match(L'='))push(TokenKind::StarAssign);else push(TokenKind::Star);break;
            case L'/':{
                const auto canEndExpression=[&](){
                    if(tokens.empty())return false;const auto& previous=tokens.back();
                    if(previous.kind==TokenKind::Identifier)return previous.text!=L"return"&&previous.text!=L"throw"&&previous.text!=L"case";
                    return previous.kind==TokenKind::Number||previous.kind==TokenKind::String||
                           previous.kind==TokenKind::Template||previous.kind==TokenKind::RegExp||
                           previous.kind==TokenKind::RightParen||previous.kind==TokenKind::RightBracket;
                };
                if(canEndExpression()){if(Match(L'='))push(TokenKind::SlashAssign);else push(TokenKind::Slash);break;}
                std::wstring pattern;bool escaped=false;
                while(position_<source_.size()){
                    const wchar_t item=source_[position_++];
                    if(!escaped&&item==L'/')break;
                    pattern+=item;
                    if(escaped)escaped=false;else escaped=item==L'\\';
                }
                std::wstring flags;while(position_<source_.size()&&std::iswalpha(source_[position_]))flags+=source_[position_++];
                token.kind=TokenKind::RegExp;token.text=DecodeRegexUnicodeEscapes(pattern);token.extra=std::move(flags);tokens.push_back(std::move(token));break;
            }
            case L'%':if(Match(L'='))push(TokenKind::PercentAssign);else push(TokenKind::Percent);break;
            case L'!':
                if (Match(L'=')) { const bool strict=Match(L'='); token.kind=strict?TokenKind::StrictNotEqual:TokenKind::NotEqual; token.text=strict?L"!==":L"!="; tokens.push_back(token); }
                else push(TokenKind::Bang); break;
            case L'=':
                if (Match(L'>')) { token.kind=TokenKind::Arrow;token.text=L"=>";tokens.push_back(token); }
                else if (Match(L'=')) { const bool strict=Match(L'=');token.kind=strict?TokenKind::StrictEqual:TokenKind::Equal;token.text=strict?L"===":L"==";tokens.push_back(token); }
                else push(TokenKind::Assign); break;
            case L'<':
                if(Match(L'<')){if(Match(L'='))push(TokenKind::ShiftLeftAssign);else push(TokenKind::ShiftLeft);}
                else if(Match(L'=')){token.kind=TokenKind::LessEqual;tokens.push_back(token);}else push(TokenKind::Less);break;
            case L'>':
                if(Match(L'>')){
                    if(Match(L'>')){if(Match(L'='))push(TokenKind::UnsignedShiftRightAssign);else push(TokenKind::UnsignedShiftRight);}
                    else if(Match(L'='))push(TokenKind::ShiftRightAssign);else push(TokenKind::ShiftRight);
                }else if(Match(L'=')){token.kind=TokenKind::GreaterEqual;tokens.push_back(token);}else push(TokenKind::Greater);break;
            case L'&':
                if(Match(L'&')){if(Match(L'='))push(TokenKind::AndAssign);else push(TokenKind::And);}
                else if(Match(L'='))push(TokenKind::BitAndAssign);else push(TokenKind::BitAnd);break;
            case L'|':
                if(Match(L'|')){if(Match(L'='))push(TokenKind::OrAssign);else push(TokenKind::Or);}
                else if(Match(L'='))push(TokenKind::BitOrAssign);else push(TokenKind::BitOr);break;
            case L'^':if(Match(L'='))push(TokenKind::BitXorAssign);else push(TokenKind::BitXor);break;
            case L'~':push(TokenKind::BitNot);break;
            default: break;
            }
        }
        return tokens;
    }
private:
    bool Match(wchar_t c){if(position_<source_.size()&&source_[position_]==c){++position_;return true;}return false;}
    void SkipTrivia(){
        for(;;){
            while(position_<source_.size()&&std::iswspace(source_[position_])){if(source_[position_++]==L'\n')++line_;}
            if(position_+1<source_.size()&&source_[position_]==L'/'&&source_[position_+1]==L'/'){
                position_+=2;while(position_<source_.size()&&source_[position_]!=L'\n')++position_;continue;
            }
            if(position_+1<source_.size()&&source_[position_]==L'/'&&source_[position_+1]==L'*'){
                position_+=2;while(position_+1<source_.size()&&!(source_[position_]==L'*'&&source_[position_+1]==L'/')){if(source_[position_++]==L'\n')++line_;}if(position_+1<source_.size())position_+=2;continue;
            }
            break;
        }
    }
    void AppendEscape(std::wstring& out){
        if(position_>=source_.size())return;
        const wchar_t escaped=source_[position_++];
        switch(escaped){
        case L'n':out+=L'\n';return;case L'r':out+=L'\r';return;
        case L't':out+=L'\t';return;case L'b':out+=L'\b';return;
        case L'f':out+=L'\f';return;case L'v':out+=L'\v';return;
        case L'0':out+=L'\0';return;
        case L'\n':++line_;return;
        case L'\r':if(position_<source_.size()&&source_[position_]==L'\n')++position_;++line_;return;
        case L'x':{
            if(position_+2<=source_.size()){
                const int high=HexDigitValue(source_[position_]),low=HexDigitValue(source_[position_+1]);
                if(high>=0&&low>=0){out+=static_cast<wchar_t>((high<<4)|low);position_+=2;return;}
            }
            out+=L'x';return;
        }
        case L'u':{
            size_t unicodePosition=position_-1;std::uint32_t codePoint=0;
            if(ParseUnicodeEscape(source_,unicodePosition,codePoint)){
                AppendCodePoint(out,codePoint);position_=unicodePosition;return;
            }
            out+=L'u';return;
        }
        default:out+=escaped;return;
        }
    }
    std::wstring ReadString(wchar_t quote){
        std::wstring out;
        while(position_<source_.size()){
            wchar_t c=source_[position_++]; if(c==quote)break;
            if(c==L'\\')AppendEscape(out);else{if(c==L'\n')++line_;out+=c;}
        }return out;
    }
    std::wstring ReadTemplate(){
        std::wstring out; int interpolation=0;
        while(position_<source_.size()){
            wchar_t c=source_[position_++];
            if(c==L'\\'&&position_<source_.size()){
                if(interpolation==0)AppendEscape(out);
                else{out+=c;out+=source_[position_++];}
                continue;
            }
            if(c==L'`'&&interpolation==0)break;
            if(c==L'$'&&position_<source_.size()&&source_[position_]==L'{'){out+=c;out+=source_[position_++];++interpolation;continue;}
            if(c==L'{'&&interpolation>0)++interpolation;
            if(c==L'}'&&interpolation>0)--interpolation;
            out+=c;
        }return out;
    }
    const std::wstring& source_; size_t position_=0; int line_=1;
};

class Compiler {
public:
    Compiler(std::shared_ptr<Module> module, const std::wstring& source)
        : module_(std::move(module)), tokens_(Lexer(source).Scan()) {}
    Chunk CompileProgram(){ Chunk chunk; chunk_=&chunk; while(!AtEnd()) Statement(); Emit(Op::Undefined);Emit(Op::Return);return chunk; }
    Chunk CompileExpressionOnly(){Chunk chunk;chunk_=&chunk;Expression();Emit(Op::Return);return chunk;}
private:
    bool AtEnd()const{return Peek().kind==TokenKind::End;}
    const Token& Peek(int ahead=0)const{return tokens_[std::min(position_+static_cast<size_t>(ahead),tokens_.size()-1)];}
    const Token& Previous()const{return tokens_[position_-1];}
    bool Check(TokenKind kind)const{return Peek().kind==kind;}
    bool CheckWord(const wchar_t* word)const{return Check(TokenKind::Identifier)&&Peek().text==word;}
    Token Advance(){if(!AtEnd())++position_;return Previous();}
    bool Match(TokenKind kind){if(Check(kind)){Advance();return true;}return false;}
    bool MatchWord(const wchar_t* word){if(CheckWord(word)){Advance();return true;}return false;}
    Token Consume(TokenKind kind,const wchar_t* message){if(Check(kind))return Advance();throw Error(message);}
    Token ConsumeIdentifier(const wchar_t* message){return Consume(TokenKind::Identifier,message);}
    std::runtime_error Error(const wchar_t* message)const{
        std::wstringstream s;s<<L"JavaScript line "<<Peek().line<<L": "<<message;
        if(Peek().kind!=TokenKind::End&&!Peek().text.empty()){
            s<<L" near '"<<Peek().text.substr(0,48)<<L"'";
            if(position_>0&&!Previous().text.empty())s<<L" after '"<<Previous().text.substr(0,48)<<L"'";
            if(Peek(1).kind!=TokenKind::End&&!Peek(1).text.empty())s<<L" before '"<<Peek(1).text.substr(0,48)<<L"'";
        }
        return std::runtime_error(WideToUtf8(s.str()));
    }
    int Emit(Op op,int argument=0,std::wstring text=L""){chunk_->code.push_back({op,argument,std::move(text)});return static_cast<int>(chunk_->code.size()-1);}
    int Jump(Op op){return Emit(op,-1);}
    void Patch(int index){chunk_->code[index].argument=static_cast<int>(chunk_->code.size());}
    int Constant(Value v){chunk_->constants.push_back(std::move(v));return static_cast<int>(chunk_->constants.size()-1);}
    void OptionalSemicolon(){Match(TokenKind::Semicolon);}

    void Statement(){
        if(Match(TokenKind::Semicolon))return;
        if(Match(TokenKind::LeftBrace)){while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");return;}
        if(CheckWord(L"async")&&Peek(1).kind==TokenKind::Identifier&&Peek(1).text==L"function"){Advance();Advance();FunctionDeclaration(true);return;}
        if(MatchWord(L"class")){ClassDeclaration();return;}
        if(MatchWord(L"function")){FunctionDeclaration();return;}
        if(MatchWord(L"const")||MatchWord(L"let")||MatchWord(L"var")){VariableDeclaration();return;}
        if(MatchWord(L"if")){IfStatement();return;}
        if(MatchWord(L"try")){TryStatement();return;}
        if(MatchWord(L"throw")){Expression();Emit(Op::ThrowValue);OptionalSemicolon();return;}
        if(MatchWord(L"switch")){SwitchStatement();return;}
        if(MatchWord(L"for")){ForStatement();return;}
        if(MatchWord(L"do")){DoWhileStatement();return;}
        if(MatchWord(L"while")){WhileStatement();return;}
        if(MatchWord(L"break")){if(controls_.empty())throw Error(L"break outside loop or switch");controls_.back().breaks.push_back(Jump(Op::Jump));OptionalSemicolon();return;}
        if(MatchWord(L"continue")){
            auto context=std::find_if(controls_.rbegin(),controls_.rend(),[](const ControlContext& value){return value.continueTarget!=-1;});
            if(context==controls_.rend())throw Error(L"continue outside loop");
            if(context->continueTarget>=0)Emit(Op::Jump,context->continueTarget);
            else context->continues.push_back(Jump(Op::Jump));
            OptionalSemicolon();return;
        }
        if(MatchWord(L"return")){if(Check(TokenKind::Semicolon)||Check(TokenKind::RightBrace))Emit(Op::Undefined);else Expression();Emit(Op::Return);OptionalSemicolon();return;}
        Expression();Emit(Op::Pop);OptionalSemicolon();
    }
    void TryStatement(){
        const int handlerIndex=static_cast<int>(chunk_->handlers.size());
        chunk_->handlers.emplace_back();
        Chunk::ExceptionHandler handler;
        handler.tryStart=chunk_->code.size();
        Statement();
        handler.tryEnd=chunk_->code.size();
        const int afterTry=Jump(Op::Jump);
        int afterCatch=-1;
        if(MatchWord(L"catch")){
            handler.hasCatch=true;
            if(Match(TokenKind::LeftParen)){handler.catchName=ConsumeIdentifier(L"Expected catch variable").text;Consume(TokenKind::RightParen,L"Expected ')'");}
            handler.catchStart=chunk_->code.size();
            Statement();
            handler.catchEnd=chunk_->code.size();
            Emit(Op::LeaveCatch,handlerIndex);
            afterCatch=Jump(Op::Jump);
        }
        if(MatchWord(L"finally")){
            handler.hasFinally=true;
            handler.finallyStart=chunk_->code.size();
            Statement();
            Emit(Op::EndFinally,handlerIndex);
            handler.finallyEnd=chunk_->code.size();
        }
        if(!handler.hasCatch&&!handler.hasFinally)throw Error(L"Expected catch or finally");
        handler.end=chunk_->code.size();
        chunk_->code[afterTry].argument=static_cast<int>(handler.hasFinally?handler.finallyStart:handler.end);
        if(afterCatch>=0)chunk_->code[afterCatch].argument=static_cast<int>(handler.hasFinally?handler.finallyStart:handler.end);
        chunk_->handlers[handlerIndex]=std::move(handler);
    }
    void FunctionDeclaration(bool isAsync=false){
        const auto name=ConsumeIdentifier(L"Expected function name").text;
        const int index=ParseFunction(false,L"",isAsync); Emit(Op::MakeFunction,index);Emit(Op::Declare,0,name);
    }
    void ClassDeclaration(){
        const auto name=ConsumeIdentifier(L"Expected class name").text;
        Consume(TokenKind::LeftBrace,L"Expected '{' after class name");
        Emit(Op::NewObject);
        while(!Check(TokenKind::RightBrace)&&!AtEnd()){
            const bool isStatic=MatchWord(L"static");
            const bool isAsync=MatchWord(L"async");
            const auto member=ConsumeIdentifier(L"Expected class member name").text;
            if(Check(TokenKind::LeftParen)){
                const int index=ParseFunction(false,L"",isAsync);
                Emit(Op::MakeFunction,index);
                Emit(Op::ObjectSet,0,isStatic?member:L"$method:"+member);
            }else{
                if(isAsync)throw Error(L"Expected '(' after async class method");
                if(!isStatic)throw Error(L"Instance class fields are not supported");
                if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
                Emit(Op::ObjectSet,0,member);OptionalSemicolon();
            }
        }
        Consume(TokenKind::RightBrace,L"Expected '}' after class body");
        Emit(Op::Declare,0,name);
    }
    void VariableDeclaration(){
        do{
            if(Check(TokenKind::LeftBracket)||Check(TokenKind::LeftBrace)){
                const bool indexed=Match(TokenKind::LeftBracket);
                if(!indexed)Consume(TokenKind::LeftBrace,L"Expected '{'");
                const auto end=indexed?TokenKind::RightBracket:TokenKind::RightBrace;
                std::vector<std::pair<std::wstring,std::wstring>> bindings;
                while(!Check(end)&&!AtEnd()){
                    auto property=indexed?std::to_wstring(bindings.size()):ConsumeIdentifier(L"Expected property name").text;
                    auto name=indexed?ConsumeIdentifier(L"Expected element name").text:property;
                    if(!indexed&&Match(TokenKind::Colon))name=ConsumeIdentifier(L"Expected binding name").text;
                    bindings.emplace_back(property,name);
                    if(!Match(TokenKind::Comma))break;
                }
                Consume(end,L"Expected closing destructuring bracket");
                if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
                const auto source=L"$binding_"+std::to_wstring(hiddenVariable_++);
                Emit(Op::Declare,0,source);
                for(const auto& binding:bindings){
                    Emit(Op::LoadReference,0,source);
                    if(indexed){Emit(Op::Constant,Constant(Value::String(binding.first)));Emit(Op::GetIndex);}
                    else Emit(Op::GetProperty,0,binding.first);
                    Emit(Op::Declare,0,binding.second);
                }
                continue;
            }
            const auto name=ConsumeIdentifier(L"Expected variable name").text;
            if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
            Emit(Op::Declare,0,name);
        }while(Match(TokenKind::Comma));OptionalSemicolon();
    }
    void IfStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");Expression();Consume(TokenKind::RightParen,L"Expected ')'");
        const int falseJump=Jump(Op::JumpFalse);Statement();
        if(MatchWord(L"else")){const int end=Jump(Op::Jump);Patch(falseJump);Statement();Patch(end);}else Patch(falseJump);
    }
    struct ControlContext { int continueTarget=-1;std::vector<int> breaks;std::vector<int> continues; };
    void FinishControl(){
        auto context=std::move(controls_.back());controls_.pop_back();
        for(int index:context.breaks)Patch(index);
        for(int index:context.continues)chunk_->code[index].argument=context.continueTarget;
    }
    void SwitchStatement(){
        struct Clause { bool isDefault=false;size_t expressionStart=0,expressionEnd=0,bodyStart=0,bodyEnd=0;int entryJump=-1,bodyEntry=-1; };
        Consume(TokenKind::LeftParen,L"Expected '(' after switch");Expression();Consume(TokenKind::RightParen,L"Expected ')' after switch value");
        const auto valueName=L"$switch_value_"+std::to_wstring(hiddenVariable_++);Emit(Op::Declare,0,valueName);
        Consume(TokenKind::LeftBrace,L"Expected '{' after switch value");
        std::vector<Clause> clauses;
        while(!Check(TokenKind::RightBrace)&&!AtEnd()){
            Clause clause;
            if(MatchWord(L"case")){
                clause.expressionStart=position_;int parens=0,brackets=0,braces=0,questions=0;
                while(!AtEnd()){
                    const auto kind=Peek().kind;
                    if(kind==TokenKind::LeftParen)++parens;else if(kind==TokenKind::RightParen)--parens;
                    else if(kind==TokenKind::LeftBracket)++brackets;else if(kind==TokenKind::RightBracket)--brackets;
                    else if(kind==TokenKind::LeftBrace)++braces;else if(kind==TokenKind::RightBrace)--braces;
                    else if(parens==0&&brackets==0&&braces==0&&kind==TokenKind::Question)++questions;
                    else if(parens==0&&brackets==0&&braces==0&&kind==TokenKind::Colon){if(questions>0)--questions;else break;}
                    Advance();
                }
                clause.expressionEnd=position_;Consume(TokenKind::Colon,L"Expected ':' after switch case");
            }else if(MatchWord(L"default")){
                clause.isDefault=true;Consume(TokenKind::Colon,L"Expected ':' after switch default");
            }else throw Error(L"Expected case or default in switch");
            clause.bodyStart=position_;int braces=0;
            while(!AtEnd()){
                if(braces==0&&(Check(TokenKind::RightBrace)||CheckWord(L"case")||CheckWord(L"default")))break;
                if(Check(TokenKind::LeftBrace))++braces;else if(Check(TokenKind::RightBrace))--braces;
                Advance();
            }
            clause.bodyEnd=position_;clauses.push_back(clause);
        }
        Consume(TokenKind::RightBrace,L"Expected '}' after switch");const auto afterSwitch=position_;

        int defaultIndex=-1;
        for(size_t index=0;index<clauses.size();++index){
            auto& clause=clauses[index];if(clause.isDefault){defaultIndex=static_cast<int>(index);continue;}
            Emit(Op::LoadReference,0,valueName);position_=clause.expressionStart;Expression();
            if(position_!=clause.expressionEnd)throw Error(L"Invalid switch case expression");
            Emit(Op::StrictEqual);const int noMatch=Jump(Op::JumpFalse);clause.entryJump=Jump(Op::Jump);Patch(noMatch);
        }
        const int noCaseMatched=Jump(Op::Jump);
        controls_.push_back({-1,{}});
        for(auto& clause:clauses){
            clause.bodyEntry=static_cast<int>(chunk_->code.size());
            if(clause.entryJump>=0)chunk_->code[clause.entryJump].argument=clause.bodyEntry;
            position_=clause.bodyStart;while(position_<clause.bodyEnd)Statement();
            if(position_!=clause.bodyEnd)throw Error(L"Invalid switch case body");
        }
        if(defaultIndex>=0)chunk_->code[noCaseMatched].argument=clauses[static_cast<size_t>(defaultIndex)].bodyEntry;
        else Patch(noCaseMatched);
        FinishControl();position_=afterSwitch;
    }
    void WhileStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");const int condition=static_cast<int>(chunk_->code.size());
        Expression();Consume(TokenKind::RightParen,L"Expected ')'");const int done=Jump(Op::JumpFalse);
        controls_.push_back({condition,{}});Statement();Emit(Op::Jump,condition);Patch(done);FinishControl();
    }
    void DoWhileStatement(){
        const int body=static_cast<int>(chunk_->code.size());
        controls_.push_back({-2,{}});Statement();
        if(!MatchWord(L"while"))throw Error(L"Expected 'while' after do-while body");
        const int condition=static_cast<int>(chunk_->code.size());
        controls_.back().continueTarget=condition;
        Consume(TokenKind::LeftParen,L"Expected '(' after while");Expression();
        Consume(TokenKind::RightParen,L"Expected ')' after do-while condition");
        const int done=Jump(Op::JumpFalse);Emit(Op::Jump,body);Patch(done);
        OptionalSemicolon();FinishControl();
    }
    void ForStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");
        if((CheckWord(L"const")||CheckWord(L"let")||CheckWord(L"var"))&&
           Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::Identifier&&
           (Peek(2).text==L"of"||Peek(2).text==L"in")){
            Advance();const auto itemName=Advance().text;const bool enumerateKeys=Advance().text==L"in";
            ForEachStatement(itemName,enumerateKeys,true);return;
        }
        if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::Identifier&&
           (Peek(1).text==L"of"||Peek(1).text==L"in")){
            const auto itemName=Advance().text;const bool enumerateKeys=Advance().text==L"in";
            ForEachStatement(itemName,enumerateKeys,false);return;
        }
        if(!Match(TokenKind::Semicolon)){
            if(MatchWord(L"const")||MatchWord(L"let")||MatchWord(L"var"))VariableDeclaration();
            else{Expression();Emit(Op::Pop);Consume(TokenKind::Semicolon,L"Expected ';'");}
        }
        const int condition=static_cast<int>(chunk_->code.size());
        if(Check(TokenKind::Semicolon))Emit(Op::TrueValue);else Expression();
        Consume(TokenKind::Semicolon,L"Expected ';'");const int done=Jump(Op::JumpFalse);
        const int enterBody=Jump(Op::Jump);
        const int increment=static_cast<int>(chunk_->code.size());
        if(!Check(TokenKind::RightParen)){Expression();Emit(Op::Pop);}
        Consume(TokenKind::RightParen,L"Expected ')'");Emit(Op::Jump,condition);
        Patch(enterBody);controls_.push_back({increment,{}});Statement();Emit(Op::Jump,increment);Patch(done);FinishControl();
    }
    void ForEachStatement(const std::wstring& itemName,bool enumerateKeys,bool declareItem){
        Assignment();Consume(TokenKind::RightParen,enumerateKeys?L"Expected ')' after for-in object":L"Expected ')' after for-of iterable");
        if(enumerateKeys)Emit(Op::EnumerableKeys);
        const auto suffix=std::to_wstring(hiddenVariable_++);
        const auto valuesName=L"$for_values_"+suffix,indexName=L"$for_index_"+suffix;
        Emit(Op::Declare,0,valuesName);
        Emit(Op::Constant,Constant(Value::Number(0)));Emit(Op::Declare,0,indexName);

        const int condition=static_cast<int>(chunk_->code.size());
        Emit(Op::LoadReference,0,indexName);Emit(Op::LoadReference,0,valuesName);Emit(Op::GetProperty,0,L"length");Emit(Op::Less);
        const int done=Jump(Op::JumpFalse);const int enterBody=Jump(Op::Jump);
        const int increment=static_cast<int>(chunk_->code.size());
        Emit(Op::LoadReference,0,indexName);Emit(Op::PostIncrement);Emit(Op::Pop);Emit(Op::Jump,condition);

        Patch(enterBody);
        if(!declareItem)Emit(Op::LoadReference,0,itemName);
        Emit(Op::LoadReference,0,valuesName);Emit(Op::LoadReference,0,indexName);Emit(Op::GetIndex);
        if(declareItem)Emit(Op::Declare,0,itemName);else{Emit(Op::Assign);Emit(Op::Pop);}
        controls_.push_back({increment,{}});Statement();Emit(Op::Jump,increment);Patch(done);FinishControl();
    }
    void ParseParameters(const std::shared_ptr<Prototype>& prototype){
        if(!Check(TokenKind::RightParen))do{
            if(Match(TokenKind::Ellipsis)){
                prototype->restParameter=ConsumeIdentifier(L"Expected rest parameter name").text;
                if(Check(TokenKind::Comma))throw Error(L"Rest parameter must be last");
                break;
            }
            if(Match(TokenKind::LeftBracket)||Check(TokenKind::LeftBrace)){
                const bool indexed=Previous().kind==TokenKind::LeftBracket;
                if(!indexed)Consume(TokenKind::LeftBrace,L"Expected '{'");
                const auto end=indexed?TokenKind::RightBracket:TokenKind::RightBrace;
                const auto parameter=L"$parameter_"+std::to_wstring(prototype->parameters.size());
                prototype->parameters.push_back(parameter);prototype->parameterDefaults.push_back({});
                size_t index=0;
                while(!Check(end)&&!AtEnd()){
                    auto property=indexed?std::to_wstring(index++):ConsumeIdentifier(L"Expected property name").text;
                    auto name=indexed?ConsumeIdentifier(L"Expected element name").text:property;
                    if(!indexed&&Match(TokenKind::Colon))name=ConsumeIdentifier(L"Expected binding name").text;
                    std::shared_ptr<Chunk> bindingDefault;
                    if(Match(TokenKind::Assign)){
                        bindingDefault=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=bindingDefault.get();
                        Assignment();Emit(Op::Return);chunk_=outer;
                    }
                    prototype->bindings.push_back({parameter,property,name,indexed,std::move(bindingDefault)});
                    if(!Match(TokenKind::Comma))break;
                }
                Consume(end,L"Expected closing parameter bracket");
                if(Match(TokenKind::Assign)){
                    auto parameterDefault=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=parameterDefault.get();
                    Assignment();Emit(Op::Return);chunk_=outer;
                    prototype->parameterDefaults.back()=std::move(parameterDefault);
                }
                continue;
            }
            prototype->parameters.push_back(ConsumeIdentifier(L"Expected parameter").text);
            std::shared_ptr<Chunk> defaultValue;
            if(Match(TokenKind::Assign)){
                defaultValue=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=defaultValue.get();
                Assignment();Emit(Op::Return);chunk_=outer;
            }
            prototype->parameterDefaults.push_back(std::move(defaultValue));
        }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightParen));
        Consume(TokenKind::RightParen,L"Expected ')'");
    }
    int ParseFunction(bool arrow,const std::wstring& singleParameter,bool isAsync=false,
                      const std::wstring& functionName=L""){
        auto prototype=std::make_shared<Prototype>();
        prototype->name=functionName;
        prototype->lexicalThis=arrow;
        prototype->isAsync=isAsync;
        if(!singleParameter.empty()){prototype->parameters.push_back(singleParameter);prototype->parameterDefaults.push_back({});}
        else{
            if(!arrow)Consume(TokenKind::LeftParen,L"Expected '('");
            ParseParameters(prototype);
        }
        Chunk* outer=chunk_;const bool outerAsync=compilingAsync_;chunk_=&prototype->chunk;compilingAsync_=isAsync;
        if(arrow&& !Check(TokenKind::LeftBrace)){Assignment();Emit(Op::Return);}
        else{Consume(TokenKind::LeftBrace,L"Expected function body");while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);}
        chunk_=outer;compilingAsync_=outerAsync;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    bool IsParenArrow()const{
        size_t i=position_;if(i>=tokens_.size()||tokens_[i].kind!=TokenKind::LeftParen)return false;
        int depth=0;for(;i<tokens_.size();++i){
            if(tokens_[i].kind==TokenKind::LeftParen)++depth;
            else if(tokens_[i].kind==TokenKind::RightParen&&--depth==0)
                return i+1<tokens_.size()&&tokens_[i+1].kind==TokenKind::Arrow;
        }
        return false;
    }
    void Expression(){Assignment();while(Match(TokenKind::Comma)){Emit(Op::Pop);Assignment();}}
    void Assignment(){
        Conditional();
        if(Match(TokenKind::Assign)){Assignment();Emit(Op::Assign);}
        else if(Match(TokenKind::NullishAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpNotNullishKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
        else if(Match(TokenKind::PlusAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Add);Emit(Op::Assign);}
        else if(Match(TokenKind::MinusAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Subtract);Emit(Op::Assign);}
        else if(Match(TokenKind::StarAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Multiply);Emit(Op::Assign);}
        else if(Match(TokenKind::SlashAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Divide);Emit(Op::Assign);}
        else if(Match(TokenKind::PercentAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Modulo);Emit(Op::Assign);}
        else if(Match(TokenKind::ExponentAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Power);Emit(Op::Assign);}
        else if(Match(TokenKind::BitAndAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseAnd);Emit(Op::Assign);}
        else if(Match(TokenKind::BitOrAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseOr);Emit(Op::Assign);}
        else if(Match(TokenKind::BitXorAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseXor);Emit(Op::Assign);}
        else if(Match(TokenKind::ShiftLeftAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::ShiftLeft);Emit(Op::Assign);}
        else if(Match(TokenKind::ShiftRightAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::ShiftRight);Emit(Op::Assign);}
        else if(Match(TokenKind::UnsignedShiftRightAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::UnsignedShiftRight);Emit(Op::Assign);}
        else if(Match(TokenKind::AndAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpFalseKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
        else if(Match(TokenKind::OrAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpTrueKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
    }
    void Conditional(){
        Nullish();
        if(Match(TokenKind::Question)){const int no=Jump(Op::JumpFalse);Assignment();Consume(TokenKind::Colon,L"Expected ':'");const int end=Jump(Op::Jump);Patch(no);Conditional();Patch(end);}
    }
    void Nullish(){LogicalOr();while(Match(TokenKind::Nullish)){const int jump=Jump(Op::JumpNotNullishKeep);LogicalOr();Patch(jump);}}
    void LogicalOr(){LogicalAnd();while(Match(TokenKind::Or)){const int jump=Jump(Op::JumpTrueKeep);LogicalAnd();Patch(jump);}}
    void LogicalAnd(){BitwiseOr();while(Match(TokenKind::And)){const int jump=Jump(Op::JumpFalseKeep);BitwiseOr();Patch(jump);}}
    void BitwiseOr(){BitwiseXor();while(Match(TokenKind::BitOr)){BitwiseXor();Emit(Op::BitwiseOr);}}
    void BitwiseXor(){BitwiseAnd();while(Match(TokenKind::BitXor)){BitwiseAnd();Emit(Op::BitwiseXor);}}
    void BitwiseAnd(){Equality();while(Match(TokenKind::BitAnd)){Equality();Emit(Op::BitwiseAnd);}}
    void Equality(){Comparison();while(Check(TokenKind::Equal)||Check(TokenKind::NotEqual)||Check(TokenKind::StrictEqual)||Check(TokenKind::StrictNotEqual)){const auto op=Advance().kind;Comparison();Emit(op==TokenKind::Equal?Op::Equal:op==TokenKind::NotEqual?Op::NotEqual:op==TokenKind::StrictEqual?Op::StrictEqual:Op::StrictNotEqual);}}
    void Comparison(){
        Shift();
        while(Check(TokenKind::Less)||Check(TokenKind::LessEqual)||Check(TokenKind::Greater)||Check(TokenKind::GreaterEqual)||CheckWord(L"in")){
            if(MatchWord(L"in")){Shift();Emit(Op::InValue);continue;}
            const auto op=Advance().kind;Shift();Emit(op==TokenKind::Less?Op::Less:op==TokenKind::LessEqual?Op::LessEqual:op==TokenKind::Greater?Op::Greater:Op::GreaterEqual);
        }
    }
    void Shift(){Term();while(Check(TokenKind::ShiftLeft)||Check(TokenKind::ShiftRight)||Check(TokenKind::UnsignedShiftRight)){const auto op=Advance().kind;Term();Emit(op==TokenKind::ShiftLeft?Op::ShiftLeft:op==TokenKind::ShiftRight?Op::ShiftRight:Op::UnsignedShiftRight);}}
    void Term(){Factor();while(Check(TokenKind::Plus)||Check(TokenKind::Minus)){const auto op=Advance().kind;Factor();Emit(op==TokenKind::Plus?Op::Add:Op::Subtract);}}
    void Factor(){PowerExpression();while(Check(TokenKind::Star)||Check(TokenKind::Slash)||Check(TokenKind::Percent)){const auto op=Advance().kind;PowerExpression();Emit(op==TokenKind::Star?Op::Multiply:op==TokenKind::Slash?Op::Divide:Op::Modulo);}}
    void PowerExpression(){Unary();if(Match(TokenKind::Exponent)){PowerExpression();Emit(Op::Power);}}
    void Unary(){
        if(Match(TokenKind::Bang)){Unary();Emit(Op::Not);return;}if(Match(TokenKind::Minus)){Unary();Emit(Op::Negate);return;}
        if(Match(TokenKind::Plus)){Unary();Emit(Op::Positive);return;}
        if(Match(TokenKind::BitNot)){Unary();Emit(Op::BitwiseNot);return;}
        if(Match(TokenKind::PlusPlus)){Unary();Emit(Op::PreIncrement);return;}
        if(Match(TokenKind::MinusMinus)){Unary();Emit(Op::PreDecrement);return;}
        if(MatchWord(L"await")){if(!compilingAsync_)throw Error(L"await outside async function");Unary();Emit(Op::Await);return;}
        if(MatchWord(L"delete")){Unary();Emit(Op::DeleteValue);return;}
        if(MatchWord(L"void")){Unary();Emit(Op::VoidValue);return;}
        if(MatchWord(L"new")){NewExpression();return;}
        if(MatchWord(L"typeof")){Unary();Emit(Op::TypeOf);return;}Postfix();
    }
    void Arguments(){
        Emit(Op::NewArray);
        if(!Check(TokenKind::RightParen))do{
            const bool spread=Match(TokenKind::Ellipsis);Assignment();Emit(spread?Op::ArraySpread:Op::ArrayPush);
        }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightParen));
        Consume(TokenKind::RightParen,L"Expected ')'");
    }
    void Postfix(){
        Primary();
        for(;;){
            if(Match(TokenKind::Dot)){Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);}
            else if(Match(TokenKind::OptionalChain)){
                const int value=Jump(Op::JumpNotNullishKeep);
                Emit(Op::Undefined);const int end=Jump(Op::Jump);Patch(value);
                if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
                else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
                else Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
                Patch(end);
            }
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
            else if(Match(TokenKind::PlusPlus))Emit(Op::PostIncrement);
            else if(Match(TokenKind::MinusMinus))Emit(Op::PostDecrement);
            else break;
        }
    }
    void NewExpression(){
        Primary();
        while(true){
            if(Match(TokenKind::Dot))Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else break;
        }
        if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::ConstructArray);}else Emit(Op::Construct,0);
        for(;;){
            if(Match(TokenKind::Dot))Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
            else break;
        }
    }
    void Primary(){
        if(Match(TokenKind::Number)){Emit(Op::Constant,Constant(Value::Number(Previous().number)));return;}
        if(Match(TokenKind::String)){Emit(Op::Constant,Constant(Value::String(Previous().text)));return;}
        if(Match(TokenKind::Template)){Emit(Op::Template,Constant(Value::String(Previous().text)));return;}
        if(Match(TokenKind::RegExp)){auto expression=std::make_shared<Object>();expression->kind=ObjectKind::RegExp;expression->props[L"$pattern"]=Value::String(Previous().text);expression->props[L"$flags"]=Value::String(Previous().extra);Emit(Op::Constant,Constant(Value::FromObject(expression)));return;}
        if(MatchWord(L"true")){Emit(Op::TrueValue);return;}if(MatchWord(L"false")){Emit(Op::FalseValue);return;}
        if(MatchWord(L"null")){Emit(Op::Null);return;}if(MatchWord(L"undefined")){Emit(Op::Undefined);return;}
        if(MatchWord(L"async")){
            if(MatchWord(L"function")){const auto name=Check(TokenKind::Identifier)?Advance().text:L"";Emit(Op::MakeFunction,ParseFunction(false,L"",true,name));return;}
            if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::Arrow){const auto name=Advance().text;Advance();Emit(Op::MakeFunction,ParseArrowSingle(name,true));return;}
            if(IsParenArrow()){Emit(Op::MakeFunction,ParseParenArrow(true));return;}
            Primary();return;
        }
        if(MatchWord(L"function")){const auto name=Check(TokenKind::Identifier)?Advance().text:L"";Emit(Op::MakeFunction,ParseFunction(false,L"",false,name));return;}
        if(IsParenArrow()){Emit(Op::MakeFunction,ParseParenArrow(false));return;}
        if(Match(TokenKind::LeftParen)){Expression();Consume(TokenKind::RightParen,L"Expected ')'");return;}
        if(Match(TokenKind::LeftBracket)){
            Emit(Op::NewArray);
            while(!Check(TokenKind::RightBracket)&&!AtEnd()){
                if(Match(TokenKind::Comma)){Emit(Op::Undefined);Emit(Op::ArrayPush);continue;}
                const bool spread=Match(TokenKind::Ellipsis);Assignment();Emit(spread?Op::ArraySpread:Op::ArrayPush);
                if(!Match(TokenKind::Comma))break;
            }
            Consume(TokenKind::RightBracket,L"Expected ']'");return;
        }
        if(Match(TokenKind::LeftBrace)){
            Emit(Op::NewObject);
            if(!Check(TokenKind::RightBrace))do{
                if(Match(TokenKind::Ellipsis)){Assignment();Emit(Op::ObjectSpread);continue;}
                if(CheckWord(L"get")&&Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen){
                    Advance();const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L""));Emit(Op::ObjectSet,0,L"$get:"+name);continue;
                }
                if(CheckWord(L"async")&&Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen){
                    Advance();const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L"",true));Emit(Op::ObjectSet,0,name);continue;
                }
                if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::LeftParen){
                    const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L""));Emit(Op::ObjectSet,0,name);continue;
                }
                Token key;if(Check(TokenKind::Identifier)||Check(TokenKind::String)||Check(TokenKind::Number))key=Advance();else throw Error(L"Expected object key");
                if(Match(TokenKind::Colon))Assignment();
                else if(key.kind==TokenKind::Identifier)Emit(Op::LoadReference,0,key.text);
                else throw Error(L"Expected ':'");
                Emit(Op::ObjectSet,0,key.text);
            }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightBrace));
            Consume(TokenKind::RightBrace,L"Expected '}'");return;
        }
        if(Check(TokenKind::Identifier)){
            auto name=Advance().text;
            if(Match(TokenKind::Arrow)){Emit(Op::MakeFunction,ParseArrowSingle(name,false));return;}
            Emit(Op::LoadReference,0,name);return;
        }
        throw Error(L"Expected expression");
    }
    int ParseArrowSingle(const std::wstring& name,bool isAsync){
        auto prototype=std::make_shared<Prototype>();prototype->parameters.push_back(name);prototype->parameterDefaults.push_back({});prototype->lexicalThis=true;prototype->isAsync=isAsync;Chunk* outer=chunk_;const bool outerAsync=compilingAsync_;chunk_=&prototype->chunk;compilingAsync_=isAsync;
        if(Match(TokenKind::LeftBrace)){while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);}else{Assignment();Emit(Op::Return);}
        chunk_=outer;compilingAsync_=outerAsync;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    int ParseParenArrow(bool isAsync){
        Consume(TokenKind::LeftParen,L"");auto prototype=std::make_shared<Prototype>();ParseParameters(prototype);Consume(TokenKind::Arrow,L"Expected =>");
        prototype->lexicalThis=true;prototype->isAsync=isAsync;Chunk* outer=chunk_;const bool outerAsync=compilingAsync_;chunk_=&prototype->chunk;compilingAsync_=isAsync;
        if(Match(TokenKind::LeftBrace)){while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);}else{Assignment();Emit(Op::Return);}
        chunk_=outer;compilingAsync_=outerAsync;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    std::shared_ptr<Module> module_;std::vector<Token> tokens_;size_t position_=0;Chunk* chunk_=nullptr;std::vector<ControlContext> controls_;size_t hiddenVariable_=0;bool compilingAsync_=false;
};

std::wstring NumberString(double number) {
    if(std::isnan(number))return L"NaN";if(std::isinf(number))return number<0?L"-Infinity":L"Infinity";
    std::wostringstream out;out<<std::setprecision(15)<<number;auto value=out.str();
    if(value.find(L'.')!=std::wstring::npos){while(!value.empty()&&value.back()==L'0')value.pop_back();if(!value.empty()&&value.back()==L'.')value.pop_back();}
    return value;
}

void AppendEscapedHtml(std::wstring& output,const std::wstring& value,bool attribute=false){
    for(const auto character:value){
        if(character==L'&')output+=L"&amp;";
        else if(character==L'<')output+=L"&lt;";
        else if(character==L'>')output+=L"&gt;";
        else if(attribute&&character==L'\"')output+=L"&quot;";
        else output+=character;
    }
}

bool IsVoidHtmlElement(const std::wstring& tag){
    static constexpr const wchar_t* names[]={L"area",L"base",L"br",L"col",L"embed",L"hr",L"img",L"input",L"link",L"meta",L"param",L"source",L"track",L"wbr"};
    return std::find(std::begin(names),std::end(names),tag)!=std::end(names);
}

void AppendOuterHtml(std::wstring& output,const std::shared_ptr<Node>& node){
    if(!node)return;
    if(node->type==NodeType::Text){AppendEscapedHtml(output,node->text);return;}
    if(node->type==NodeType::Document){for(const auto& child:node->children)AppendOuterHtml(output,child);return;}
    output+=L'<'+node->tag;
    for(const auto& attribute:node->attributes){
        output+=L' '+attribute.first;
        output+=L"=\"";AppendEscapedHtml(output,attribute.second,true);output+=L'\"';
    }
    if(node->checked&&node->attributes.count(L"checked")==0)output+=L" checked=\"\"";
    if(node->disabled&&node->attributes.count(L"disabled")==0)output+=L" disabled=\"\"";
    output+=L'>';
    if(IsVoidHtmlElement(node->tag))return;
    for(const auto& child:node->children)AppendOuterHtml(output,child);
    output+=L"</"+node->tag+L'>';
}

std::wstring InnerHtml(const std::shared_ptr<Node>& node){
    std::wstring output;if(node)for(const auto& child:node->children)AppendOuterHtml(output,child);return output;
}

std::shared_ptr<Node> CloneDomNode(const std::shared_ptr<Node>& source,bool deep){
    if(!source)return {};
    auto clone=std::make_shared<Node>();clone->type=source->type;clone->tag=source->tag;clone->text=source->text;
    clone->attributes=source->attributes;clone->inlineStyle=source->inlineStyle;clone->files=source->files;
    clone->checked=source->checked;clone->indeterminate=source->indeterminate;clone->disabled=source->disabled;
    clone->scrollLeft=source->scrollLeft;clone->scrollTop=source->scrollTop;
    clone->selectionStart=source->selectionStart;clone->selectionEnd=source->selectionEnd;
    clone->selectionDirection=source->selectionDirection;
    if(deep)for(const auto& child:source->children){auto copied=CloneDomNode(child,true);copied->parent=clone;clone->children.push_back(std::move(copied));}
    return clone;
}

bool NormalizeDomNode(const std::shared_ptr<Node>& node){
    if(!node||node->type==NodeType::Text)return false;
    bool changed=false;
    for(auto& child:node->children)changed=NormalizeDomNode(child)||changed;
    for(size_t index=0;index<node->children.size();){
        auto& child=node->children[index];
        if(child->type==NodeType::Text&&child->text.empty()&&node->children.size()>1){child->parent.reset();node->children.erase(node->children.begin()+static_cast<std::ptrdiff_t>(index));changed=true;continue;}
        if(index+1<node->children.size()&&child->type==NodeType::Text&&node->children[index+1]->type==NodeType::Text){
            child->text+=node->children[index+1]->text;node->children[index+1]->parent.reset();node->children.erase(node->children.begin()+static_cast<std::ptrdiff_t>(index+1));changed=true;continue;
        }
        ++index;
    }
    return changed;
}

std::wstring CamelToKebab(const std::wstring& value){std::wstring out;for(wchar_t c:value){if(std::iswupper(c)){out+=L'-';out+=std::towlower(c);}else out+=c;}return out;}
std::wstring DatasetAttributeName(const std::wstring& value){return L"data-"+CamelToKebab(value);}
bool AttributeRequiresLayout(const std::wstring& name){
    return name==L"value"||name==L"placeholder"||name==L"type"||name==L"rows"||
           name==L"cols"||name==L"size"||name==L"multiple"||name==L"colspan"||
           name==L"rowspan"||name==L"span"||name==L"width"||name==L"height"||
           name==L"lang"||name==L"src";
}
void ResetImageSourceState(const std::shared_ptr<Node>& node){
    if(!node||node->tag!=L"img")return;
    node->image.reset();node->imageSource=node->Attribute(L"src");
    node->imageComplete=node->imageSource.empty();
}

struct RuntimeCore {
    struct EventListener {
        std::uint64_t id=0;
        Value callback;
        bool capture=false;
        bool once=false;
        bool passive=false;
    };
    using EventListenerMap=FastMap<std::wstring,std::vector<EventListener>>;
    struct MutationBatch {
        RuntimeCore& runtime;
        explicit MutationBatch(RuntimeCore& value):runtime(value){runtime.BeginMutationBatch();}
        ~MutationBatch(){runtime.EndMutationBatch();}
    };
    Document& document;
    EditingCommandExecutor editingCommands;
    JavaScriptRuntime::MessageSink messageSink;
    JavaScriptRuntime::MutationSink mutationSink;
    JavaScriptRuntime::GeometryProvider geometryProvider;
    JavaScriptRuntime::StylePropertyProvider stylePropertyProvider;
    JavaScriptRuntime::FrameScheduler frameScheduler;
    JavaScriptRuntime::TimerScheduler timerScheduler;
    JavaScriptRuntime::ResourceLoader resourceLoader;
    JavaScriptRuntime::DialogSink dialogSink;
    JavaScriptRuntime::FrameMessageSink frameMessageSink;
    JavaScriptRuntime::ParentMessageSink parentMessageSink;
    JavaScriptRuntime::FocusSink focusSink;
    JavaScriptRuntime::ActivationSink activationSink;
    JavaScriptRuntime::PointerCaptureSink pointerCaptureSink;
    JavaScriptRuntime::SelectionProvider selectionProvider;
    JavaScriptRuntime::SelectionSetter selectionSetter;
    JavaScriptRuntime::DomSelectionProvider domSelectionProvider;
    JavaScriptRuntime::DomSelectionSetter domSelectionSetter;
    std::wstring location;
    std::shared_ptr<Environment> global;
    std::shared_ptr<Module> module=std::make_shared<Module>();
    std::unordered_map<const Object*,std::weak_ptr<Object>> managedObjects;
    std::unordered_map<const Function*,std::weak_ptr<Function>> managedFunctions;
    std::unordered_map<const NativeFunction*,std::weak_ptr<NativeFunction>> managedNativeFunctions;
    std::unordered_map<const Environment*,std::weak_ptr<Environment>> managedEnvironments;
    size_t managedAllocationsSinceSweep=0;
    struct NodeEventListeners {
        std::weak_ptr<Node> node;
        EventListenerMap events;
    };
    FastMap<Node*,NodeEventListeners> listeners;
    EventListenerMap documentListeners;
    EventListenerMap windowListeners;
    EventListenerMap webViewListeners;
    FastMap<Object*,EventListenerMap> objectListeners;
    FastMap<Node*,std::weak_ptr<Object>> nodeObjects;
    FastMap<Node*,std::weak_ptr<Object>> canvasContexts;
    FastMap<Node*,std::weak_ptr<Object>> frameWindows;
    FastMap<std::wstring,std::shared_ptr<const std::wregex>> regularExpressions;
    FastMap<std::wstring,std::shared_ptr<const FiniteRegexSeparator>> finiteRegularExpressions;
    FastMap<std::wstring,AnchoredRegexPrefixes> anchoredRegexPrefixes;
    struct TemplateProgram {
        std::vector<std::wstring> literals;
        std::vector<Chunk> expressions;
    };
    FastMap<std::wstring,std::shared_ptr<TemplateProgram>> templatePrograms;
    FastMap<std::wstring,Value> inlineHandlerCache;
    std::vector<std::pair<unsigned,Value>> frameCallbacks;
    struct TimerEntry { unsigned id=0;Value callback;std::chrono::steady_clock::time_point due;unsigned interval=0; };
    std::vector<TimerEntry> timers;
    std::deque<std::function<void()>> microtasks;
    std::vector<std::weak_ptr<Object>> possiblyUnhandledRejections;
    unsigned nextFrameId=1;
    unsigned nextTimerId=1;
    std::uint64_t nextEventListenerId=1;
    bool frameScheduled=false;
    bool timerScheduled=false;
    bool drainingMicrotasks=false;
    std::chrono::steady_clock::time_point timerWake{};
    std::wstring lastError;
    int mutationBatchDepth=0;
    bool mutationPending=false;
    JavaScriptRuntime::MutationKind mutationKind=JavaScriptRuntime::MutationKind::Paint;
    std::vector<std::shared_ptr<Node>> mutationTargets;
    bool liveRegionMembershipChanged=false;
    bool indexDirty=false;
    double viewportWidth=0;
    double viewportHeight=0;
    double devicePixelRatio=1;
    std::weak_ptr<Node> pointerCaptureNode;

    explicit RuntimeCore(Document& d)
        :document(d),
         editingCommands(
             document,
             [this](EditingSelection& selection){
                 return domSelectionProvider&&domSelectionProvider(selection);
             },
             [this](const EditingSelection& selection){
                 if(domSelectionSetter)domSelectionSetter(selection);
             },
             [this](const std::shared_ptr<Node>& target,EditingMutationKind kind,
                    bool requiresIndex){
                 Mutated(target,
                         kind==EditingMutationKind::Tree?
                             JavaScriptRuntime::MutationKind::Tree:
                             JavaScriptRuntime::MutationKind::Style,
                         requiresIndex);
             },
             [this](const std::shared_ptr<Node>& node){ExecuteConnectedScripts(node);}){
        global=CreateEnvironment();InstallGlobals();
    }
    ~RuntimeCore(){ReleaseManagedGraph();}

    template<class Map>
    static void PruneExpiredManagedEntries(Map& entries){
        std::vector<typename Map::key_type> expired;
        for(const auto& entry:entries)if(entry.second.expired())expired.push_back(entry.first);
        for(const auto& key:expired)entries.erase(key);
    }
    void MaybePruneManagedEntries(){
        if(++managedAllocationsSinceSweep<256)return;
        managedAllocationsSinceSweep=0;
        PruneExpiredManagedEntries(managedObjects);
        PruneExpiredManagedEntries(managedFunctions);
        PruneExpiredManagedEntries(managedNativeFunctions);
        PruneExpiredManagedEntries(managedEnvironments);
        PruneExpiredManagedEntries(nodeObjects);
        PruneExpiredManagedEntries(canvasContexts);
        PruneExpiredManagedEntries(frameWindows);
        // Event listeners belong to their target node.  Keeping the callback
        // in a raw-pointer keyed map after a detached target has died turns the
        // listener into an unintended runtime root and leaves a dangling key.
        // Connected nodes and detached nodes still referenced by JavaScript
        // remain alive through this weak association and keep their listeners.
        std::vector<Node*> expiredListenerTargets;
        for(const auto& entry:listeners)
            if(entry.second.node.expired())expiredListenerTargets.push_back(entry.first);
        for(auto* target:expiredListenerTargets)listeners.erase(target);
    }
    template<class T>
    void TrackManaged(std::unordered_map<const T*,std::weak_ptr<T>>& entries,
                      const std::shared_ptr<T>& value){
        if(!value)return;
        const auto found=entries.find(value.get());
        if(found==entries.end()){
            entries.emplace(value.get(),value);MaybePruneManagedEntries();
        }else if(found->second.expired()){
            found->second=value;MaybePruneManagedEntries();
        }
    }
    std::shared_ptr<Object> CreateObject(ObjectKind kind=ObjectKind::Plain){
        auto object=std::make_shared<Object>();object->kind=kind;
        TrackManaged(managedObjects,object);return object;
    }
    void TrackObject(const std::shared_ptr<Object>& object){
        TrackManaged(managedObjects,object);
    }
    void TrackValue(const Value& value){
        TrackManaged(managedObjects,value.object);
        TrackManaged(managedFunctions,value.function);
        TrackManaged(managedNativeFunctions,value.native);
        if(value.reference&&value.reference->env)
            TrackManaged(managedEnvironments,value.reference->env);
    }
    std::shared_ptr<Function> CreateFunction(){
        auto function=std::make_shared<Function>();TrackManaged(managedFunctions,function);return function;
    }
    std::shared_ptr<Environment> CreateEnvironment(){
        auto environment=std::make_shared<Environment>();TrackManaged(managedEnvironments,environment);return environment;
    }
    void ReleaseManagedGraph()noexcept{
        // JavaScript values form arbitrary graphs (window.parent points back to
        // window, closures point back to their environment, and user objects may
        // point to themselves). shared_ptr cannot collect those graphs, so sever
        // every engine-owned edge before dropping a page or the runtime itself.
        for(auto& entry:managedNativeFunctions)if(auto native=entry.second.lock())native->callback={};
        for(auto& entry:managedFunctions)if(auto function=entry.second.lock()){
            function->closure.reset();function->prototype.reset();
        }
        for(auto& entry:managedEnvironments)if(auto environment=entry.second.lock()){
            environment->values.clear();environment->parent.reset();
        }
        for(auto& entry:managedObjects)if(auto object=entry.second.lock()){
            object->props.clear();object->items.clear();object->entries.clear();
            object->prototype.reset();object->promiseResult=Value::Undefined();
            object->promiseReactions.clear();object->node.reset();object->canvasGradient.reset();
        }
        managedNativeFunctions.clear();managedFunctions.clear();
        managedEnvironments.clear();managedObjects.clear();
        managedAllocationsSinceSweep=0;
    }
    void ResetExecutionState(bool notifyTimerScheduler){
        if(notifyTimerScheduler&&timerScheduler)timerScheduler(0);
        ReleaseManagedGraph();
        listeners.clear();documentListeners.clear();windowListeners.clear();webViewListeners.clear();
        objectListeners.clear();pointerCaptureNode.reset();
        if(pointerCaptureSink)pointerCaptureSink(false);
        nodeObjects.clear();canvasContexts.clear();frameWindows.clear();inlineHandlerCache.clear();
        frameCallbacks.clear();frameScheduled=false;timers.clear();timerScheduled=false;
        microtasks.clear();possiblyUnhandledRejections.clear();templatePrograms.clear();mutationTargets.clear();
        module=std::make_shared<Module>();global=CreateEnvironment();InstallGlobals();
        for(const auto& script:document.QuerySelectorAll(L"script"))script->scriptStarted=true;
    }

    std::shared_ptr<const std::wregex> RegularExpression(const std::wstring& pattern,
                                                        const std::wstring& flags){
        const auto key=flags+L'\x1f'+pattern;
        const auto cached=regularExpressions.find(key);
        if(cached!=regularExpressions.end())return cached->second;
        std::wstring compatiblePattern;
        if(!TranslateUnicodePropertyEscapes(pattern,flags,compatiblePattern))return {};
        std::wregex::flag_type options=std::regex_constants::ECMAScript;
        if(flags.find(L'i')!=std::wstring::npos)options|=std::regex_constants::icase;
        try{
            auto expression=std::make_shared<const std::wregex>(compatiblePattern,options);
            if(regularExpressions.size()>=256)regularExpressions.clear();
            regularExpressions.emplace(key,expression);
            return expression;
        }catch(...){return {};}
    }

    std::shared_ptr<const FiniteRegexSeparator> FiniteRegularExpression(
        const std::wstring& pattern,const std::wstring& flags){
        const auto key=flags+L'\x1f'+pattern;
        const auto cached=finiteRegularExpressions.find(key);
        if(cached!=finiteRegularExpressions.end())return cached->second;
        auto expression=std::make_shared<FiniteRegexSeparator>();
        if(!CompileFiniteRegexSeparator(pattern,flags,*expression))expression.reset();
        if(finiteRegularExpressions.size()>=256)finiteRegularExpressions.clear();
        finiteRegularExpressions.emplace(key,expression);
        return expression;
    }

    bool RegularExpressionMayMatch(const std::wstring& pattern,const std::wstring& flags,
                                   const std::wstring& input){
        const auto key=flags+L'\x1f'+pattern;
        auto cached=anchoredRegexPrefixes.find(key);
        if(cached==anchoredRegexPrefixes.end()){
            if(anchoredRegexPrefixes.size()>=256)anchoredRegexPrefixes.clear();
            cached=anchoredRegexPrefixes.emplace(key,CompileAnchoredRegexPrefixes(pattern,flags)).first;
        }
        return cached->second.MayMatch(input);
    }

    Value Deref(const Value& input){
        if(input.type!=Value::Type::Reference)return input;
        const auto& ref=*input.reference;
        if(ref.kind==Reference::Kind::Variable){
            auto* env=ref.env?ref.env->Find(ref.name):nullptr;return env?env->values[ref.name]:Value::Undefined();
        }
        return GetProperty(Deref(ref.base),ref.name);
    }
    Value Deref(Value&& input){
        if(input.type!=Value::Type::Reference)return std::move(input);
        return Deref(static_cast<const Value&>(input));
    }
    bool Truth(const Value& value){
        const auto v=Deref(value);switch(v.type){case Value::Type::Undefined:case Value::Type::Null:return false;case Value::Type::Boolean:return v.boolean;case Value::Type::Number:return v.number!=0&&!std::isnan(v.number);case Value::Type::String:return !v.string.empty();default:return true;}
    }
    double Number(const Value& value){
        const auto v=Deref(value);if(v.type==Value::Type::Number)return v.number;if(v.type==Value::Type::Boolean)return v.boolean?1:0;if(v.type==Value::Type::Null)return 0;if(v.type==Value::Type::String){const auto text=Trim(v.string);if(text.empty())return 0;wchar_t* end=nullptr;const double number=std::wcstod(text.c_str(),&end);if(end==text.c_str()||!end||*end!=L'\0')return std::numeric_limits<double>::quiet_NaN();return number;}if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::Date&&v.object->props.count(L"$time"))return Number(v.object->props[L"$time"]);return std::numeric_limits<double>::quiet_NaN();
    }
    std::uint32_t Uint32(const Value& value){
        const double number=Number(value);if(!std::isfinite(number)||number==0)return 0;
        double reduced=std::fmod(std::trunc(number),4294967296.0);if(reduced<0)reduced+=4294967296.0;
        return static_cast<std::uint32_t>(reduced);
    }
    std::int32_t Int32(const Value& value){return static_cast<std::int32_t>(Uint32(value));}
    bool HasProperty(const Value& input,const std::wstring& key){
        const auto value=Deref(input);
        if(value.type==Value::Type::String){
            if(key==L"length")return true;
            size_t index=0;return TryParseDecimalIndex(key,index)&&index<value.string.size();
        }
        if(value.type!=Value::Type::Object||!value.object)return false;
        const auto object=value.object;
        if(object->props.count(key)||object->props.count(L"$get:"+key))return true;
        if(object->kind==ObjectKind::Array){
            if(key==L"length")return true;
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<object->items.size())return true;
        }
        for(auto prototype=object->prototype;prototype;prototype=prototype->prototype)
            if(prototype->props.count(key)||prototype->props.count(L"$get:"+key)||prototype->props.count(L"$method:"+key))return true;
        return false;
    }
    std::wstring String(const Value& value){
        const auto v=Deref(value);switch(v.type){case Value::Type::Undefined:return L"undefined";case Value::Type::Null:return L"null";case Value::Type::Boolean:return v.boolean?L"true":L"false";case Value::Type::Number:return NumberString(v.number);case Value::Type::String:return v.string;case Value::Type::Function:case Value::Type::Native:return L"function";case Value::Type::Object:if(v.object&&v.object->kind==ObjectKind::Array)return L"[object Array]";if(v.object&&v.object->kind==ObjectKind::Promise)return L"[object Promise]";if(v.object&&v.object->kind==ObjectKind::Error){const auto name=v.object->props.find(L"name"),message=v.object->props.find(L"message");const auto n=name==v.object->props.end()?L"Error":String(name->second);const auto m=message==v.object->props.end()?L"":String(message->second);return m.empty()?n:n+L": "+m;}return L"[object Object]";default:return L"undefined";}
    }
    Value NodeValue(const std::shared_ptr<Node>& node){
        if(!node)return Value::Null();auto found=nodeObjects.find(node.get());if(found!=nodeObjects.end())if(auto cached=found->second.lock())return Value::FromObject(cached);
        auto object=CreateObject(ObjectKind::Node);object->node=node;nodeObjects[node.get()]=object;return Value::FromObject(object);
    }
    Value RangeValue(const std::shared_ptr<Node>& start,size_t startOffset,
                     const std::shared_ptr<Node>& end,size_t endOffset){
        auto range=CreateObject(ObjectKind::Range);range->rangeStart=start;range->rangeStartOffset=startOffset;
        range->rangeEnd=end;range->rangeEndOffset=endOffset;return Value::FromObject(range);
    }
    static std::shared_ptr<Node> CommonAncestor(std::shared_ptr<Node> first,
                                                std::shared_ptr<Node> second){
        std::vector<std::shared_ptr<Node>> ancestors;
        for(auto current=first;current;current=current->parent.lock())ancestors.push_back(current);
        for(auto current=second;current;current=current->parent.lock())
            if(std::find(ancestors.begin(),ancestors.end(),current)!=ancestors.end())return current;
        return {};
    }
    static size_t DomTextLength(const std::shared_ptr<Node>& node){
        if(!node)return 0;if(node->type==NodeType::Text)return node->text.size();
        size_t length=0;for(const auto& child:node->children)length+=DomTextLength(child);return length;
    }
    static bool BoundaryTextOffset(const std::shared_ptr<Node>& root,
                                   const std::shared_ptr<Node>& container,size_t offset,
                                   size_t& cursor,size_t& result){
        if(!root)return false;
        if(root==container){
            if(root->type==NodeType::Text)result=cursor+std::min(offset,root->text.size());
            else{
                const size_t count=std::min(offset,root->children.size());
                size_t local=0;for(size_t index=0;index<count;++index)local+=DomTextLength(root->children[index]);
                result=cursor+local;
            }
            return true;
        }
        if(root->type==NodeType::Text){cursor+=root->text.size();return false;}
        for(const auto& child:root->children)if(BoundaryTextOffset(child,container,offset,cursor,result))return true;
        return false;
    }
    static std::wstring RangeText(const std::shared_ptr<Object>& range){
        if(!range||!range->rangeStart||!range->rangeEnd)return {};
        const auto root=CommonAncestor(range->rangeStart,range->rangeEnd);if(!root)return {};
        size_t cursor=0,start=0,end=0;
        if(!BoundaryTextOffset(root,range->rangeStart,range->rangeStartOffset,cursor,start))return {};
        cursor=0;if(!BoundaryTextOffset(root,range->rangeEnd,range->rangeEndOffset,cursor,end))return {};
        if(end<start)std::swap(start,end);const auto text=root->InnerText();
        return text.substr(std::min(start,text.size()),std::min(end,text.size())-std::min(start,text.size()));
    }
    static bool IsDocumentFragment(const std::shared_ptr<Node>& node){
        return node&&node->type==NodeType::Element&&node->tag==L"#document-fragment";
    }
    static std::shared_ptr<Node> NextNodeWithin(const std::shared_ptr<Node>& current,
                                                const std::shared_ptr<Node>& root){
        if(!current||!root)return {};
        if(!current->children.empty())return current->children.front();
        for(auto node=current;node&&node!=root;node=node->parent.lock()){
            const auto parent=node->parent.lock();if(!parent)return {};
            const auto found=std::find(parent->children.begin(),parent->children.end(),node);
            if(found!=parent->children.end()&&std::next(found)!=parent->children.end())return *std::next(found);
        }
        return {};
    }
    void DeleteRangeContents(const std::shared_ptr<Object>& range){
        if(!range||!range->rangeStart||!range->rangeEnd)return;
        auto start=range->rangeStart,end=range->rangeEnd;
        size_t startOffset=range->rangeStartOffset,endOffset=range->rangeEndOffset;
        if(start==end){
            if(start->type==NodeType::Text){
                startOffset=std::min(startOffset,start->text.size());endOffset=std::min(endOffset,start->text.size());
                if(endOffset<startOffset)std::swap(startOffset,endOffset);
                start->text.erase(startOffset,endOffset-startOffset);
            }else{
                startOffset=std::min(startOffset,start->children.size());endOffset=std::min(endOffset,start->children.size());
                if(endOffset<startOffset)std::swap(startOffset,endOffset);
                for(size_t index=startOffset;index<endOffset;++index){document.UnindexSubtree(start->children[index]);start->children[index]->parent.reset();}
                start->children.erase(start->children.begin()+static_cast<std::ptrdiff_t>(startOffset),
                                      start->children.begin()+static_cast<std::ptrdiff_t>(endOffset));
            }
            range->rangeEnd=start;range->rangeEndOffset=startOffset;
            Mutated(start,JavaScriptRuntime::MutationKind::Tree);return;
        }
        const auto root=CommonAncestor(start,end);if(!root)return;
        std::vector<std::shared_ptr<Node>> textNodes;
        std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& node){
            if(node->type==NodeType::Text){textNodes.push_back(node);return;}
            for(const auto& child:node->children)collect(child);
        };collect(root);
        const auto first=std::find(textNodes.begin(),textNodes.end(),start);
        const auto last=std::find(textNodes.begin(),textNodes.end(),end);
        if(first!=textNodes.end()&&last!=textNodes.end()){
            auto begin=first,finish=last;size_t beginOffset=startOffset,finishOffset=endOffset;
            if(begin>finish){std::swap(begin,finish);std::swap(beginOffset,finishOffset);}
            beginOffset=std::min(beginOffset,(*begin)->text.size());
            finishOffset=std::min(finishOffset,(*finish)->text.size());
            (*begin)->text.erase(beginOffset);
            for(auto iterator=std::next(begin);iterator!=finish;++iterator)(*iterator)->text.clear();
            (*finish)->text.erase(0,finishOffset);
            range->rangeStart=*begin;range->rangeStartOffset=beginOffset;
            range->rangeEnd=*begin;range->rangeEndOffset=beginOffset;
            Mutated(root,JavaScriptRuntime::MutationKind::Tree);
        }
    }
    void InsertRangeNode(const std::shared_ptr<Object>& range,const std::shared_ptr<Node>& inserted){
        if(!range||!range->rangeStart||!inserted)return;
        auto container=range->rangeStart;size_t offset=range->rangeStartOffset;
        std::shared_ptr<Node> parent;size_t insertion=0;
        if(container->type==NodeType::Text){
            parent=container->parent.lock();if(!parent)return;
            const auto found=std::find(parent->children.begin(),parent->children.end(),container);if(found==parent->children.end())return;
            insertion=static_cast<size_t>(found-parent->children.begin());offset=std::min(offset,container->text.size());
            if(offset>0){
                auto left=std::make_shared<Node>();left->type=NodeType::Text;left->tag=L"#text";left->text=container->text.substr(0,offset);left->parent=parent;left->ownerDocument=container->ownerDocument;
                *found=left;++insertion;
            }else parent->children.erase(found);
            container->text.erase(0,offset);
            if(!container->text.empty()){container->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insertion),container);}
        }else{
            parent=container;insertion=std::min(offset,parent->children.size());
        }
        std::vector<std::shared_ptr<Node>> nodes;
        if(IsDocumentFragment(inserted)){nodes=inserted->children;inserted->children.clear();}
        else nodes.push_back(inserted);
        bool indexOk=true;
        for(auto& node:nodes){
            if(auto old=node->parent.lock()){
                indexOk=document.UnindexSubtree(node)&&indexOk;
                old->children.erase(std::remove(old->children.begin(),old->children.end(),node),old->children.end());
            }
            node->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insertion++),node);
            indexOk=document.IndexSubtree(node)&&indexOk;ExecuteConnectedScripts(node);
        }
        Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);
    }
    Value SelectionValue(){
        auto selection=CreateObject(ObjectKind::Selection);JavaScriptRuntime::DomSelection current;
        if(domSelectionProvider&&domSelectionProvider(current)&&current.anchorNode&&current.focusNode)
            selection->props[L"$range"]=RangeValue(current.anchorNode,current.anchorOffset,
                                                    current.focusNode,current.focusOffset);
        return Value::FromObject(selection);
    }
    static unsigned CanvasDimension(const std::shared_ptr<Node>& node,const wchar_t* name,
                                    unsigned fallback){
        if(!node)return fallback;
        const auto raw=Trim(node->Attribute(name));if(raw.empty())return fallback;
        size_t used=0;double number=0;
        if(!TryParseDouble(raw,number,&used)||used!=raw.size()||!std::isfinite(number)||number<0)
            return fallback;
        return static_cast<unsigned>(std::min(65535.0,std::floor(number)));
    }
    std::shared_ptr<CanvasSurface> EnsureCanvas(const std::shared_ptr<Node>& node){
        if(!node)return {};
        const unsigned width=CanvasDimension(node,L"width",300);
        const unsigned height=CanvasDimension(node,L"height",150);
        if(!node->canvas){node->canvas=std::make_shared<CanvasSurface>();node->canvas->Reset(width,height);}
        else if(node->canvas->width!=width||node->canvas->height!=height)
            node->canvas->Reset(width,height);
        return node->canvas;
    }
    std::shared_ptr<CanvasSurface> ResetCanvas(const std::shared_ptr<Node>& node){
        if(!node)return {};
        if(!node->canvas)node->canvas=std::make_shared<CanvasSurface>();
        node->canvas->Reset(CanvasDimension(node,L"width",300),
                            CanvasDimension(node,L"height",150));
        return node->canvas;
    }
    Value CanvasContextValue(const std::shared_ptr<Node>& node){
        if(!node||node->tag!=L"canvas")return Value::Null();
        auto found=canvasContexts.find(node.get());
        if(found!=canvasContexts.end())if(auto cached=found->second.lock()){
            EnsureCanvas(node);return Value::FromObject(cached);
        }
        EnsureCanvas(node);auto object=CreateObject(ObjectKind::CanvasContext2D);object->node=node;
        canvasContexts[node.get()]=object;return Value::FromObject(object);
    }
    Value FrameWindowValue(const std::shared_ptr<Node>& node){
        if(!node)return Value::Null();
        auto found=frameWindows.find(node.get());
        if(found!=frameWindows.end())if(auto cached=found->second.lock())return Value::FromObject(cached);
        auto object=CreateObject(ObjectKind::FrameWindow);object->node=node;
        frameWindows[node.get()]=object;return Value::FromObject(object);
    }
    Value ArrayValue(const std::vector<Value>& items){auto o=CreateObject(ObjectKind::Array);o->items=items;return Value::FromObject(o);}
    Value CloneValue(const Value& input){
        const auto value=Deref(input);if(value.type!=Value::Type::Object||!value.object)return value;
        if(value.object->kind==ObjectKind::Array){std::vector<Value> items;items.reserve(value.object->items.size());for(const auto& item:value.object->items)items.push_back(CloneValue(item));return ArrayValue(items);}
        if(value.object->kind==ObjectKind::Date)return DateValue({value.object->props[L"$time"]});
        if(value.object->kind!=ObjectKind::Plain)return value;
        auto clone=ObjectValue(ObjectKind::Plain);for(const auto& property:value.object->props)clone.object->props[property.first]=CloneValue(property.second);return clone;
    }
    static double CurrentTimeMilliseconds(){
        using namespace std::chrono;return duration<double,std::milli>(system_clock::now().time_since_epoch()).count();
    }
    static double ParseDateMilliseconds(const std::wstring& value){
        SYSTEMTIME time{};wchar_t separator=0;int consumed=0;
        const int matched=swscanf_s(value.c_str(),L"%hu-%hu-%hu%c%hu:%hu:%hu%n",
            &time.wYear,&time.wMonth,&time.wDay,&separator,1,&time.wHour,&time.wMinute,&time.wSecond,&consumed);
        if(matched<7||(separator!=L'T'&&separator!=L't'&&separator!=L' '))
            return std::numeric_limits<double>::quiet_NaN();
        size_t index=consumed>0?static_cast<size_t>(consumed):value.size();
        time.wMilliseconds=0;
        if(index<value.size()&&value[index]==L'.'){
            ++index;unsigned milliseconds=0,digits=0;
            while(index<value.size()&&std::iswdigit(value[index])){
                if(digits<3)milliseconds=milliseconds*10+static_cast<unsigned>(value[index]-L'0');
                ++digits;++index;
            }
            while(digits<3){milliseconds*=10;++digits;}
            time.wMilliseconds=static_cast<WORD>(milliseconds);
        }
        bool explicitZone=false;int offsetMinutes=0;
        if(index<value.size()&&(value[index]==L'Z'||value[index]==L'z')){
            explicitZone=true;++index;
        }else if(index<value.size()&&(value[index]==L'+'||value[index]==L'-')){
            explicitZone=true;const int direction=value[index++]==L'+'?1:-1;
            auto twoDigits=[&](int& result){
                if(index+1>=value.size()||!std::iswdigit(value[index])||!std::iswdigit(value[index+1]))return false;
                result=(value[index]-L'0')*10+(value[index+1]-L'0');index+=2;return true;
            };
            int hours=0,minutes=0;if(!twoDigits(hours))return std::numeric_limits<double>::quiet_NaN();
            if(index<value.size()&&value[index]==L':')++index;
            if(!twoDigits(minutes)||hours>23||minutes>59)return std::numeric_limits<double>::quiet_NaN();
            offsetMinutes=direction*(hours*60+minutes);
        }
        while(index<value.size()&&std::iswspace(value[index]))++index;
        if(index!=value.size())return std::numeric_limits<double>::quiet_NaN();
        SYSTEMTIME utc=time;
        if(!explicitZone&&!TzSpecificLocalTimeToSystemTime(nullptr,&time,&utc))
            return std::numeric_limits<double>::quiet_NaN();
        FILETIME file{};if(!SystemTimeToFileTime(&utc,&file))return std::numeric_limits<double>::quiet_NaN();
        ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;
        long long adjusted=static_cast<long long>(ticks.QuadPart)-
            static_cast<long long>(offsetMinutes)*60ll*10000000ll;
        constexpr unsigned long long epoch=116444736000000000ull;
        return adjusted<static_cast<long long>(epoch)?std::numeric_limits<double>::quiet_NaN():
            static_cast<double>(adjusted-static_cast<long long>(epoch))/10000.0;
    }
    static bool DateSystemTime(double milliseconds,SYSTEMTIME& time,bool local){
        if(!std::isfinite(milliseconds))return false;constexpr unsigned long long epoch=116444736000000000ull;
        const auto ticks=static_cast<long long>(std::trunc(milliseconds*10000.0))+static_cast<long long>(epoch);if(ticks<0)return false;
        ULARGE_INTEGER value{};value.QuadPart=static_cast<unsigned long long>(ticks);FILETIME utc{value.LowPart,value.HighPart},converted=utc;
        if(local&&!FileTimeToLocalFileTime(&utc,&converted))return false;return FileTimeToSystemTime(&converted,&time)!=FALSE;
    }
    Value DateValue(const std::vector<Value>& args){
        double milliseconds=CurrentTimeMilliseconds();if(!args.empty()){const auto value=Deref(args[0]);milliseconds=value.type==Value::Type::String?ParseDateMilliseconds(value.string):Number(value);}
        auto date=ObjectValue(ObjectKind::Date);date.object->props[L"$time"]=Value::Number(milliseconds);return date;
    }
    Value FileValue(const Node::FileInfo& info){
        auto file=ObjectValue(ObjectKind::File);
        file.object->props[L"name"]=Value::String(info.name);
        file.object->props[L"type"]=Value::String(info.type);
        file.object->props[L"path"]=Value::String(info.path);
        file.object->props[L"$path"]=Value::String(info.path);
        file.object->props[L"size"]=Value::Number(static_cast<double>(info.size));
        return file;
    }
    Value FileListValue(const std::vector<Node::FileInfo>& infos){
        std::vector<Value> files;files.reserve(infos.size());
        for(const auto& info:infos)files.push_back(FileValue(info));
        return ArrayValue(files);
    }
    Value DataTransferValue(const std::wstring& text,const std::vector<Node::FileInfo>& infos){
        auto transfer=ObjectValue(ObjectKind::DataTransfer);
        transfer.object->props[L"$text"]=Value::String(text);
        transfer.object->props[L"files"]=FileListValue(infos);
        std::vector<Value> types;if(!text.empty())types.push_back(Value::String(L"text/plain"));
        if(!infos.empty())types.push_back(Value::String(L"Files"));
        transfer.object->props[L"types"]=ArrayValue(types);
        std::vector<Value> items;items.reserve(infos.size());
        for(const auto& info:infos){auto item=ObjectValue(ObjectKind::DataTransferItem);
            item.object->props[L"kind"]=Value::String(L"file");
            item.object->props[L"type"]=Value::String(info.type);
            item.object->props[L"$file"]=FileValue(info);items.push_back(item);
        }
        transfer.object->props[L"items"]=ArrayValue(items);
        return transfer;
    }
    Value Native(NativeCallback callback){auto n=std::make_shared<NativeFunction>();n->callback=std::move(callback);TrackManaged(managedNativeFunctions,n);return Value::FromNative(n);}
    Value ObjectValue(ObjectKind kind){return Value::FromObject(CreateObject(kind));}
    bool IsCallable(const Value& input){const auto value=Deref(input);return value.type==Value::Type::Function||value.type==Value::Type::Native;}
    Value ErrorValue(const std::wstring& name,const std::wstring& message){
        auto error=ObjectValue(ObjectKind::Error);error.object->props[L"name"]=Value::String(name);error.object->props[L"message"]=Value::String(message);error.object->props[L"stack"]=Value::String(message.empty()?name:name+L": "+message);return error;
    }
    Value PromiseValue(){return ObjectValue(ObjectKind::Promise);}
    void EnqueueMicrotask(std::function<void()> job){microtasks.push_back(std::move(job));}
    void SchedulePromiseReaction(const std::shared_ptr<Object>& promise,PromiseReaction reaction){
        EnqueueMicrotask([this,promise,reaction=std::move(reaction)]() mutable {
            const bool fulfilled=promise->promiseState==PromiseState::Fulfilled;
            const auto handler=fulfilled?reaction.onFulfilled:reaction.onRejected;
            if(!IsCallable(handler)){
                if(fulfilled)ResolvePromise(reaction.nextPromise,promise->promiseResult);
                else RejectPromise(reaction.nextPromise,promise->promiseResult);
                return;
            }
            try{ResolvePromise(reaction.nextPromise,Call(handler,Value::Undefined(),{promise->promiseResult}));}
            catch(const JavaScriptException& exception){RejectPromise(reaction.nextPromise,exception.value);}
            catch(const std::exception& exception){RejectPromise(reaction.nextPromise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
        });
    }
    Value PerformThen(const std::shared_ptr<Object>& promise,const Value& onFulfilled,const Value& onRejected){
        auto next=PromiseValue().object;
        if(promise->promiseUnhandledNotified){promise->promiseUnhandledNotified=false;auto* runtime=this;EnqueueMicrotask([runtime,promise](){runtime->DispatchPromiseRejectionEvent(L"rejectionhandled",promise);});}
        promise->promiseHandled=true;
        PromiseReaction reaction{Deref(onFulfilled),Deref(onRejected),next};
        if(promise->promiseState==PromiseState::Pending)promise->promiseReactions.push_back(std::move(reaction));
        else SchedulePromiseReaction(promise,std::move(reaction));
        return Value::FromObject(next);
    }
    void SettlePromise(const std::shared_ptr<Object>& promise,PromiseState state,const Value& result){
        if(!promise||promise->kind!=ObjectKind::Promise||promise->promiseState!=PromiseState::Pending)return;
        promise->promiseState=state;promise->promiseResult=Deref(result);
        auto reactions=std::move(promise->promiseReactions);promise->promiseReactions.clear();
        if(state==PromiseState::Rejected&&!promise->promiseHandled&&reactions.empty())possiblyUnhandledRejections.push_back(promise);
        for(auto& reaction:reactions)SchedulePromiseReaction(promise,std::move(reaction));
    }
    void RejectPromise(const std::shared_ptr<Object>& promise,const Value& reason){SettlePromise(promise,PromiseState::Rejected,reason);}
    void ResolvePromise(const std::shared_ptr<Object>& promise,const Value& input){
        if(!promise||promise->promiseState!=PromiseState::Pending)return;
        const auto resolution=Deref(input);
        if(resolution.type==Value::Type::Object&&resolution.object==promise){RejectPromise(promise,ErrorValue(L"TypeError",L"Chaining cycle detected for promise"));return;}
        if(resolution.type==Value::Type::Object&&resolution.object&&resolution.object->kind==ObjectKind::Promise){
            const auto source=resolution.object;source->promiseHandled=true;
            PromiseReaction adoption{Value::Undefined(),Value::Undefined(),promise};
            if(source->promiseState==PromiseState::Pending)source->promiseReactions.push_back(std::move(adoption));
            else SchedulePromiseReaction(source,std::move(adoption));
            return;
        }
        if(resolution.type==Value::Type::Object&&resolution.object){
            Value then;
            try{then=GetProperty(resolution,L"then");}catch(const JavaScriptException& exception){RejectPromise(promise,exception.value);return;}
            if(IsCallable(then)){
                auto called=std::make_shared<bool>(false);
                auto resolve=Native([promise,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.ResolvePromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
                auto reject=Native([promise,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.RejectPromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
                try{Call(then,resolution,{resolve,reject});}
                catch(const JavaScriptException& exception){if(!*called){*called=true;RejectPromise(promise,exception.value);}}
                catch(const std::exception& exception){if(!*called){*called=true;RejectPromise(promise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}}
                return;
            }
        }
        SettlePromise(promise,PromiseState::Fulfilled,resolution);
    }
    Value PromiseResolveValue(const Value& value){const auto resolved=Deref(value);if(resolved.type==Value::Type::Object&&resolved.object&&resolved.object->kind==ObjectKind::Promise)return resolved;auto promise=PromiseValue();ResolvePromise(promise.object,resolved);return promise;}
    Value ConstructPromise(const Value& executor){
        if(!IsCallable(executor))throw JavaScriptException{ErrorValue(L"TypeError",L"Promise resolver is not a function")};
        auto promise=PromiseValue();auto called=std::make_shared<bool>(false);
        auto resolve=Native([promise=promise.object,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.ResolvePromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
        auto reject=Native([promise=promise.object,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.RejectPromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
        try{Call(executor,Value::Undefined(),{resolve,reject});}
        catch(const JavaScriptException& exception){if(!*called){*called=true;RejectPromise(promise.object,exception.value);}}
        catch(const std::exception& exception){if(!*called){*called=true;RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));}}
        return promise;
    }
    void DispatchPromiseRejectionEvent(const std::wstring& type,const std::shared_ptr<Object>& promise){
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(type);event->props[L"promise"]=Value::FromObject(promise);event->props[L"reason"]=promise->promiseResult;
        const auto eventValue=Value::FromObject(event);const auto target=global->values[L"window"];
        event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        InvokeEventListeners(windowListeners,type,true,target,eventValue,event);
        InvokeEventListeners(windowListeners,type,false,target,eventValue,event);
    }
    void DrainMicrotasks(){
        if(drainingMicrotasks)return;drainingMicrotasks=true;
        for(;;){
            while(!microtasks.empty()){
                auto job=std::move(microtasks.front());microtasks.pop_front();
                try{MutationBatch batch(*this);job();}
                catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
                catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
            }
            auto pending=std::move(possiblyUnhandledRejections);possiblyUnhandledRejections.clear();
            for(const auto& weak:pending)if(auto promise=weak.lock())if(promise->promiseState==PromiseState::Rejected&&!promise->promiseHandled&&!promise->promiseUnhandledNotified){promise->promiseUnhandledNotified=true;DispatchPromiseRejectionEvent(L"unhandledrejection",promise);}
            if(microtasks.empty())break;
        }
        drainingMicrotasks=false;
    }
    void BeginMutationBatch(){++mutationBatchDepth;}
    void FlushMutations(){
        if(indexDirty){document.Reindex();indexDirty=false;}
        if(mutationPending&&mutationSink){
            JavaScriptRuntime::Mutation mutation;
            mutation.kind=mutationKind;
            mutation.targets=std::move(mutationTargets);
            mutation.liveRegionMembershipChanged=liveRegionMembershipChanged;
            mutationPending=false;mutationKind=JavaScriptRuntime::MutationKind::Paint;
            liveRegionMembershipChanged=false;
            mutationSink(mutation);
        }else{
            mutationPending=false;mutationTargets.clear();
            mutationKind=JavaScriptRuntime::MutationKind::Paint;
            liveRegionMembershipChanged=false;
        }
    }
    void EndMutationBatch(){if(mutationBatchDepth>0&&--mutationBatchDepth==0)FlushMutations();}
    void EnsureIndex(){if(indexDirty){document.Reindex();indexDirty=false;}}
    void Mutated(const std::shared_ptr<Node>& target,JavaScriptRuntime::MutationKind kind,
                 bool requiresIndex=false,bool liveRegionMembership=false){
        mutationPending=true;
        if(static_cast<int>(kind)>static_cast<int>(mutationKind))mutationKind=kind;
        if(target&&std::none_of(mutationTargets.begin(),mutationTargets.end(),
            [&](const auto& candidate){return candidate==target;}))mutationTargets.push_back(target);
        indexDirty=indexDirty||requiresIndex;
        liveRegionMembershipChanged=liveRegionMembershipChanged||liveRegionMembership;
        if(mutationBatchDepth==0)FlushMutations();
    }
    unsigned ScheduleAnimationFrame(const Value& callback){
        const unsigned id=nextFrameId++;
        frameCallbacks.emplace_back(id,Deref(callback));
        if(frameScheduler&&!frameScheduled){frameScheduled=true;frameScheduler();}
        return id;
    }
    void RunAnimationFrame(){
        frameScheduled=false;
        std::vector<std::pair<unsigned,Value>> callbacks;
        callbacks.swap(frameCallbacks);
        if(callbacks.empty())return;
        MutationBatch batch(*this);
        using namespace std::chrono;
        const auto timestamp=Value::Number(duration<double,std::milli>(steady_clock::now().time_since_epoch()).count());
        for(const auto& callback:callbacks){try{Call(callback.second,Value::Undefined(),{timestamp});}catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}DrainMicrotasks();}
    }
    void ScheduleNextTimer(){
        if(timers.empty()){timerScheduled=false;if(timerScheduler)timerScheduler(0);return;}
        const auto earliest=std::min_element(timers.begin(),timers.end(),[](const auto& a,const auto& b){return a.due<b.due;})->due;
        if(timerScheduled&&earliest>=timerWake)return;
        timerScheduled=true;timerWake=earliest;
        const auto now=std::chrono::steady_clock::now();
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(earliest-now).count();
        if(timerScheduler)timerScheduler(static_cast<unsigned>(std::max<long long>(1,remaining)));
    }
    unsigned ScheduleTimer(const Value& callback,double delay,bool repeat=false){
        const unsigned id=nextTimerId++;
        const auto milliseconds=static_cast<long long>(std::max(0.0,std::min(delay,2147483647.0)));
        timers.push_back({id,Deref(callback),std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds),repeat?static_cast<unsigned>(std::max<long long>(1,milliseconds)):0});
        ScheduleNextTimer();return id;
    }
    void ClearTimer(unsigned id){
        timers.erase(std::remove_if(timers.begin(),timers.end(),[&](const auto& timer){return timer.id==id;}),timers.end());
        timerScheduled=false;ScheduleNextTimer();
    }
    void RunTimers(){
        timerScheduled=false;const auto now=std::chrono::steady_clock::now();std::vector<Value> callbacks;
        for(auto it=timers.begin();it!=timers.end();)if(it->due<=now+std::chrono::milliseconds(1)){callbacks.push_back(it->callback);if(it->interval){it->due=now+std::chrono::milliseconds(it->interval);++it;}else it=timers.erase(it);}else ++it;
        if(!callbacks.empty()){MutationBatch batch(*this);for(const auto& callback:callbacks){try{Call(callback,Value::Undefined(),{});}catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}DrainMicrotasks();}}
        ScheduleNextTimer();
    }
    bool IsConnected(const std::shared_ptr<Node>& node)const{
        auto current=node;
        while(current&&current->parent.lock())current=current->parent.lock();
        return current&&current==document.Root();
    }
    void ExecuteConnectedScripts(const std::shared_ptr<Node>& root){
        if(!root||!IsConnected(root))return;
        std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& node){
            if(!node)return;
            if(node->type==NodeType::Element&&node->tag==L"script"&&!node->scriptStarted){
                node->scriptStarted=true;
                std::wstring source;
                const auto resource=node->Attribute(L"src");
                if(resource.empty())source=node->InnerText();
                else if(!resourceLoader||!resourceLoader(resource,source))
                    throw JavaScriptException{ErrorValue(L"Error",L"Cannot load script: "+resource)};
                if(!source.empty()){
                    Compiler compiler(module,source);
                    Run(compiler.CompileProgram(),global);
                    DrainMicrotasks();
                }
                Dispatch(node,L"load");
            }
            for(const auto& child:node->children)visit(child);
        };
        visit(root);
    }
    Value GetProperty(const Value& input,const std::wstring& key){
        const auto base=Deref(input);
        if(base.type==Value::Type::Number&&key==L"toFixed")return Native([value=base.number](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const int digits=a.empty()?0:std::max(0,std::min(100,static_cast<int>(r.Number(a[0]))));
            if(!std::isfinite(value))return Value::String(NumberString(value));
            std::wostringstream out;out<<std::fixed<<std::setprecision(digits)<<value;
            return Value::String(out.str());
        });
        if((base.type==Value::Type::Function||base.type==Value::Type::Native)&&(key==L"bind"||key==L"call")){
            return Native([callable=base,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto boundThis=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(key==L"call"){std::vector<Value> forwarded;if(a.size()>1)forwarded.assign(a.begin()+1,a.end());return r.Call(callable,boundThis,forwarded);}
                return r.Native([callable,boundThis](RuntimeCore& inner,const Value&,const std::vector<Value>& args){
                    return inner.Call(callable,boundThis,args);
                });
            });
        }
        if(base.type==Value::Type::String){
            if(key==L"length")return Value::Number(static_cast<double>(base.string.size()));
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<base.string.size())return Value::String(std::wstring(1,base.string[index]));
            if(key==L"indexOf")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto found=s.find(a.empty()?L"":r.String(a[0]),a.size()>1?static_cast<size_t>(std::max(0.0,r.Number(a[1]))):0);return Value::Number(found==std::wstring::npos?-1.0:static_cast<double>(found));});
            if(key==L"lastIndexOf")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto needle=a.empty()?L"":r.String(a[0]);size_t position=std::wstring::npos;if(a.size()>1){const auto number=r.Number(a[1]);if(!std::isnan(number)){const auto integer=std::trunc(number);position=static_cast<size_t>(std::max(0.0,std::min(static_cast<double>(s.size()),integer)));}}const auto found=s.rfind(needle,position);return Value::Number(found==std::wstring::npos?-1.0:static_cast<double>(found));});
            if(key==L"toLowerCase"||key==L"toLocaleLowerCase"||key==L"toUpperCase")return Native([s=base.string,key](RuntimeCore&,const Value&,const std::vector<Value>&){auto out=s;std::transform(out.begin(),out.end(),out.begin(),[&](wchar_t c){return key==L"toUpperCase"?std::towupper(c):std::towlower(c);});return Value::String(out);});
            if(key==L"trim")return Native([s=base.string](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(Trim(s));});
            if(key==L"trimStart")return Native([s=base.string](RuntimeCore&,const Value&,const std::vector<Value>&){const auto begin=std::find_if_not(s.begin(),s.end(),[](wchar_t character){return std::iswspace(character)!=0;});return Value::String(std::wstring(begin,s.end()));});
            if(key==L"startsWith")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto prefix=a.empty()?L"":r.String(a[0]);return Value::Bool(s.rfind(prefix,0)==0);});
            if(key==L"endsWith")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto suffix=a.empty()?L"":r.String(a[0]);size_t end=s.size();if(a.size()>1)end=static_cast<size_t>(std::max(0.0,std::min(static_cast<double>(s.size()),r.Number(a[1]))));return Value::Bool(suffix.size()<=end&&s.compare(end-suffix.size(),suffix.size(),suffix)==0);});
            if(key==L"repeat")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto count=a.empty()?0ll:static_cast<long long>(r.Number(a[0]));if(count<0||count>1000000)throw JavaScriptException{r.ErrorValue(L"RangeError",L"Invalid count value")};std::wstring result;result.reserve(s.size()*static_cast<size_t>(count));for(long long index=0;index<count;++index)result+=s;return Value::String(std::move(result));});
            if(key==L"slice")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto size=static_cast<long long>(s.size());auto offset=[&](size_t i,long long fallback){if(i>=a.size())return fallback;auto n=static_cast<long long>(r.Number(a[i]));return n<0?std::max(0ll,size+n):std::min(size,n);};const auto begin=offset(0,0),end=offset(1,size);return Value::String(s.substr(static_cast<size_t>(begin),static_cast<size_t>(std::max(0ll,end-begin))));});
            if(key==L"split")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<Value> out;
                const size_t limit=a.size()>1?static_cast<size_t>(r.Uint32(a[1])):
                    static_cast<size_t>((std::numeric_limits<std::uint32_t>::max)());
                if(limit==0)return r.ArrayValue(out);
                const auto separatorValue=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(separatorValue.type==Value::Type::Undefined){out.push_back(Value::String(s));return r.ArrayValue(out);}
                auto append=[&](const std::wstring& value){if(out.size()<limit)out.push_back(Value::String(value));};
                if(separatorValue.type==Value::Type::Object&&separatorValue.object&&
                   separatorValue.object->kind==ObjectKind::RegExp){
                    const auto pattern=r.String(separatorValue.object->props[L"$pattern"]);
                    const auto flags=r.String(separatorValue.object->props[L"$flags"]);
                    if(const auto finiteSeparator=r.FiniteRegularExpression(pattern,flags)){
                        size_t lastEnd=0,searchStart=0;
                        bool matchedEmptyInput=false;
                        while(searchStart<=s.size()&&out.size()<limit){
                            size_t matchStart=searchStart,matchLength=0;
                            for(;matchStart<=s.size();++matchStart)
                                if(finiteSeparator->MatchAt(s,matchStart,matchLength))break;
                            if(matchStart>s.size())break;
                            if(matchLength==0&&matchStart==lastEnd){
                                matchedEmptyInput=matchedEmptyInput||s.empty();
                                if(matchStart>=s.size())break;
                                searchStart=matchStart+1;
                                continue;
                            }
                            if(matchLength==0&&matchStart==s.size())break;
                            append(s.substr(lastEnd,matchStart-lastEnd));
                            lastEnd=matchStart+matchLength;
                            searchStart=matchLength==0?matchStart+1:lastEnd;
                        }
                        if(out.size()<limit&&!(s.empty()&&matchedEmptyInput))append(s.substr(lastEnd));
                        return r.ArrayValue(out);
                    }
                    if(const auto expression=r.RegularExpression(pattern,flags)){
                        size_t lastEnd=0;
                        bool matchedEmptyInput=false;
                        for(std::wsregex_iterator iterator(s.begin(),s.end(),*expression),end;
                            iterator!=end&&out.size()<limit;++iterator){
                            const auto& match=*iterator;
                            const size_t matchStart=static_cast<size_t>(match.position());
                            const size_t matchEnd=matchStart+static_cast<size_t>(match.length());
                            if(match.length()==0&&matchStart==lastEnd){
                                matchedEmptyInput=matchedEmptyInput||s.empty();
                                continue;
                            }
                            if(match.length()==0&&matchStart==s.size())break;
                            append(s.substr(lastEnd,matchStart-lastEnd));
                            for(size_t capture=1;capture<match.size()&&out.size()<limit;++capture)
                                append(match[capture].matched?match[capture].str():L"");
                            lastEnd=matchEnd;
                        }
                        if(out.size()<limit&&!(s.empty()&&matchedEmptyInput))append(s.substr(lastEnd));
                        return r.ArrayValue(out);
                    }else{out.push_back(Value::String(s));return r.ArrayValue(out);}
                }
                const auto separator=r.String(separatorValue);
                if(separator.empty()){
                    for(wchar_t character:s){append(std::wstring(1,character));if(out.size()>=limit)break;}
                }else{
                    size_t start=0;
                    for(;;){
                        const auto end=s.find(separator,start);
                        append(s.substr(start,end==std::wstring::npos?std::wstring::npos:end-start));
                        if(end==std::wstring::npos||out.size()>=limit)break;
                        start=end+separator.size();
                    }
                }
                return r.ArrayValue(out);
            });
            if(key==L"match")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto patternValue=a.empty()?Value::Undefined():r.Deref(a[0]);
                std::wstring pattern,flags;
                if(patternValue.type==Value::Type::Object&&patternValue.object&&
                   patternValue.object->kind==ObjectKind::RegExp){
                    pattern=r.String(patternValue.object->props[L"$pattern"]);
                    flags=r.String(patternValue.object->props[L"$flags"]);
                }else pattern=patternValue.type==Value::Type::Undefined?L"":r.String(patternValue);
                if(!r.RegularExpressionMayMatch(pattern,flags,s))return Value::Null();
                if(const auto expression=r.RegularExpression(pattern,flags)){
                    std::vector<Value> matches;
                    if(flags.find(L'g')!=std::wstring::npos){
                        for(std::wsregex_iterator it(s.begin(),s.end(),*expression),end;it!=end;++it)
                            matches.push_back(Value::String((*it)[0].str()));
                        return matches.empty()?Value::Null():r.ArrayValue(matches);
                    }
                    std::wsmatch match;
                    if(!std::regex_search(s,match,*expression))return Value::Null();
                    for(const auto& capture:match)
                        matches.push_back(capture.matched?Value::String(capture.str()):Value::Undefined());
                    auto result=r.ArrayValue(matches);
                    result.object->props[L"index"]=Value::Number(static_cast<double>(match.position()));
                    result.object->props[L"input"]=Value::String(s);
                    return result;
                }else return Value::Null();
            });
            if(key==L"matchAll")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto patternValue=a.empty()?Value::Undefined():r.Deref(a[0]);std::wstring pattern,flags;
                if(patternValue.type==Value::Type::Object&&patternValue.object&&patternValue.object->kind==ObjectKind::RegExp){pattern=r.String(patternValue.object->props[L"$pattern"]);flags=r.String(patternValue.object->props[L"$flags"]);}else pattern=patternValue.type==Value::Type::Undefined?L"":r.String(patternValue);
                std::vector<Value> results;if(!r.RegularExpressionMayMatch(pattern,flags,s))return r.ArrayValue(results);
                if(const auto expression=r.RegularExpression(pattern,flags))for(std::wsregex_iterator iterator(s.begin(),s.end(),*expression),end;iterator!=end;++iterator){const auto& match=*iterator;std::vector<Value> captures;for(const auto& capture:match)captures.push_back(capture.matched?Value::String(capture.str()):Value::Undefined());auto item=r.ArrayValue(captures);item.object->props[L"index"]=Value::Number(static_cast<double>(match.position()));item.object->props[L"input"]=Value::String(s);results.push_back(item);}
                return r.ArrayValue(results);
            });
            if(key==L"padStart")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){size_t n=a.empty()?0:static_cast<size_t>(r.Number(a[0]));std::wstring fill=a.size()>1?r.String(a[1]):L" ";if(fill.empty())fill=L" ";std::wstring out=s;while(out.size()<n)out=fill+out;if(out.size()>n)out=out.substr(out.size()-n);return Value::String(out);});
            if(key==L"replaceAll")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto search=a.empty()?L"":r.String(a[0]);const auto replacement=a.size()>1?r.String(a[1]):L"undefined";
                if(search.empty()){std::wstring out=replacement;for(wchar_t c:s){out+=c;out+=replacement;}return Value::String(out);}
                auto match=s.find(search);if(match==std::wstring::npos)return Value::String(s);
                std::wstring out;out.reserve(s.size()+replacement.size());size_t position=0;
                do{
                    out.append(s,position,match-position);out+=replacement;
                    position=match+search.size();match=s.find(search,position);
                }while(match!=std::wstring::npos);
                out.append(s,position,std::wstring::npos);return Value::String(std::move(out));
            });
            if(key==L"replace")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::String(s);const auto pattern=r.Deref(a[0]);const auto replacement=a.size()>1?r.Deref(a[1]):Value::Undefined();
                if(pattern.type==Value::Type::Object&&pattern.object&&pattern.object->kind==ObjectKind::RegExp){
                    const auto patternText=r.String(pattern.object->props[L"$pattern"]),flags=r.String(pattern.object->props[L"$flags"]);
                    if(!r.RegularExpressionMayMatch(patternText,flags,s))return Value::String(s);
                    if(const auto expression=r.RegularExpression(patternText,flags)){std::wstring out;size_t cursor=0;for(std::wsregex_iterator it(s.begin(),s.end(),*expression),end;it!=end;++it){const auto& match=*it;const auto matchPosition=static_cast<size_t>(match.position()),matchLength=static_cast<size_t>(match.length());out+=s.substr(cursor,matchPosition-cursor);if(replacement.type==Value::Type::Function||replacement.type==Value::Type::Native){std::vector<Value> arguments;for(size_t i=0;i<match.size();++i)arguments.push_back(Value::String(match[i].str()));arguments.push_back(Value::Number(static_cast<double>(matchPosition)));arguments.push_back(Value::String(s));out+=r.String(r.Call(replacement,Value::Undefined(),arguments));}else{const auto replacementText=r.String(replacement);for(size_t index=0;index<replacementText.size();++index){const wchar_t character=replacementText[index];if(character!=L'$'||index+1>=replacementText.size()){out+=character;continue;}const wchar_t token=replacementText[index+1];if(token==L'$'){out+=L'$';++index;}else if(token==L'&'){out+=match[0].str();++index;}else if(token==L'`'){out+=s.substr(0,matchPosition);++index;}else if(token==L'\''){out+=s.substr(matchPosition+matchLength);++index;}else if(token>=L'1'&&token<=L'9'){size_t capture=static_cast<size_t>(token-L'0'),used=1;if(index+2<replacementText.size()&&replacementText[index+2]>=L'0'&&replacementText[index+2]<=L'9'){const size_t two=capture*10+static_cast<size_t>(replacementText[index+2]-L'0');if(two<match.size()){capture=two;used=2;}}if(capture<match.size()){if(match[capture].matched)out+=match[capture].str();index+=used;}else out+=L'$';}else out+=L'$';} }cursor=matchPosition+matchLength;if(flags.find(L'g')==std::wstring::npos)break;}out+=s.substr(cursor);return Value::String(out);}return Value::String(s);
                }
                const auto needle=r.String(pattern);const auto position=s.find(needle);if(position==std::wstring::npos)return Value::String(s);auto out=s;out.replace(position,needle.size(),r.String(replacement));return Value::String(out);
            });
            if(key==L"at")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto size=static_cast<long long>(s.size());auto index=a.empty()?0ll:static_cast<long long>(r.Number(a[0]));if(index<0)index+=size;return index>=0&&index<size?Value::String(std::wstring(1,s[static_cast<size_t>(index)])):Value::Undefined();});
            if(key==L"localeCompare")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto other=a.empty()?L"undefined":r.String(a[0]);const int result=CompareStringEx(LOCALE_NAME_USER_DEFAULT,NORM_LINGUISTIC_CASING,s.c_str(),static_cast<int>(s.size()),other.c_str(),static_cast<int>(other.size()),nullptr,nullptr,0);return Value::Number(result==CSTR_LESS_THAN?-1:result==CSTR_GREATER_THAN?1:0);});
        }
        if(base.type==Value::Type::Number){
            if(key==L"toFixed")return Native([n=base.number](RuntimeCore& r,const Value&,const std::vector<Value>& a){int d=a.empty()?0:static_cast<int>(r.Number(a[0]));std::wostringstream out;out<<std::fixed<<std::setprecision(std::max(0,std::min(20,d)))<<n;return Value::String(out.str());});
            if(key==L"toLocaleString")return Native([n=base.number](RuntimeCore&,const Value&,const std::vector<Value>&){
                auto value=NumberString(n);const auto exponent=value.find_first_of(L"eE");if(exponent!=std::wstring::npos)return Value::String(value);
                const auto decimal=value.find(L'.');const size_t end=decimal==std::wstring::npos?value.size():decimal;const size_t begin=!value.empty()&&(value[0]==L'-'||value[0]==L'+')?1:0;
                for(size_t position=end;position>begin+3;position-=3)value.insert(position-3,1,L',');return Value::String(value);
            });
        }
        if(base.type!=Value::Type::Object||!base.object)return Value::Undefined();
        auto object=base.object;
        // Array length is a non-configurable data property backed by the
        // indexed storage.  It must not be shadowed by an ordinary property,
        // especially after code clears a history stack with array.length = 0.
        if(object->kind==ObjectKind::Array&&key==L"length")
            return Value::Number(static_cast<double>(object->items.size()));
        const auto own=object->props.find(key);if(own!=object->props.end())return own->second;
        const auto getter=object->props.find(L"$get:"+key);if(getter!=object->props.end())return Call(getter->second,base,{});
        for(auto prototype=object->prototype;prototype;prototype=prototype->prototype){
            const auto method=prototype->props.find(L"$method:"+key);
            if(method!=prototype->props.end())return method->second;
        }
        if(object->kind==ObjectKind::DateConstructor&&key==L"now")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(CurrentTimeMilliseconds());});
        if(object->kind==ObjectKind::Date){
            const auto milliseconds=Number(object->props[L"$time"]);
            if(key==L"getTime"||key==L"valueOf")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(milliseconds);});
            if(key==L"toISOString"||key==L"toJSON")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){SYSTEMTIME time{};if(!DateSystemTime(milliseconds,time,false))return Value::String(L"Invalid Date");wchar_t buffer[40]{};swprintf_s(buffer,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);return Value::String(buffer);});
            if(key==L"toLocaleTimeString")return Native([milliseconds](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                SYSTEMTIME time{};
                if(!DateSystemTime(milliseconds,time,true))return Value::String(L"Invalid Date");
                bool force24Hour=false;
                if(a.size()>1){
                    const auto options=r.Deref(a[1]);
                    if(options.type==Value::Type::Object&&options.object){
                        const auto hour12=options.object->props.find(L"hour12");
                        force24Hour=hour12!=options.object->props.end()&&
                            r.Deref(hour12->second).type==Value::Type::Boolean&&
                            !r.Deref(hour12->second).boolean;
                    }
                }
                wchar_t buffer[128]{};
                if(force24Hour){
                    swprintf_s(buffer,L"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
                    return Value::String(buffer);
                }
                const auto locale=a.empty()?L"":r.String(a[0]);
                if(GetTimeFormatEx(locale.empty()?LOCALE_NAME_USER_DEFAULT:locale.c_str(),0,&time,nullptr,
                                   buffer,static_cast<int>(std::size(buffer)))>0)
                    return Value::String(buffer);
                swprintf_s(buffer,L"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
                return Value::String(buffer);
            });
        }
        if(object->kind==ObjectKind::Promise){
            if(key==L"then")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PerformThen(object,a.empty()?Value::Undefined():a[0],a.size()>1?a[1]:Value::Undefined());});
            if(key==L"catch")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PerformThen(object,Value::Undefined(),a.empty()?Value::Undefined():a[0]);});
            if(key==L"finally")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto callback=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(!r.IsCallable(callback))return r.PerformThen(object,Value::Undefined(),Value::Undefined());
                auto fulfilled=r.Native([callback](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){
                    const auto original=values.empty()?Value::Undefined():values[0];
                    const auto waited=runtime.PromiseResolveValue(runtime.Call(callback,Value::Undefined(),{}));
                    auto restore=runtime.Native([original](RuntimeCore&,const Value&,const std::vector<Value>&){return original;});
                    return runtime.PerformThen(waited.object,restore,Value::Undefined());
                });
                auto rejected=r.Native([callback](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){
                    const auto reason=values.empty()?Value::Undefined():values[0];
                    const auto waited=runtime.PromiseResolveValue(runtime.Call(callback,Value::Undefined(),{}));
                    auto rethrow=runtime.Native([reason](RuntimeCore&,const Value&,const std::vector<Value>&)->Value{throw JavaScriptException{reason};});
                    return runtime.PerformThen(waited.object,rethrow,Value::Undefined());
                });
                return r.PerformThen(object,fulfilled,rejected);
            });
        }
        if(object->kind==ObjectKind::Error&&key==L"toString")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto name=object->props.count(L"name")?r.String(object->props[L"name"]):L"Error";const auto message=object->props.count(L"message")?r.String(object->props[L"message"]):L"";return Value::String(message.empty()?name:name+L": "+message);});
        if(object->kind==ObjectKind::Array){
            if(key==L"push")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>& args){for(auto& v:args)object->items.push_back(v);return Value::Number(static_cast<double>(object->items.size()));});
            if(key==L"unshift")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>& args){object->items.insert(object->items.begin(),args.begin(),args.end());return Value::Number(static_cast<double>(object->items.size()));});
            if(key==L"pop")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){if(object->items.empty())return Value::Undefined();auto last=object->items.back();object->items.pop_back();return last;});
            if(key==L"shift")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){if(object->items.empty())return Value::Undefined();auto first=object->items.front();object->items.erase(object->items.begin());return first;});
            if(key==L"indexOf")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Number(-1);for(size_t i=0;i<object->items.size();++i)if(r.EqualValues(object->items[i],args[0]))return Value::Number(static_cast<double>(i));return Value::Number(-1);});
            if(key==L"includes")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Bool(false);for(const auto& item:object->items)if(r.EqualValues(item,args[0]))return Value::Bool(true);return Value::Bool(false);});
            if(key==L"find")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Undefined();for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return object->items[i];return Value::Undefined();});
            if(key==L"findIndex")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Number(-1);for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return Value::Number(static_cast<double>(i));return Value::Number(-1);});
            if(key==L"findLastIndex")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Number(-1);for(size_t i=object->items.size();i>0;--i){const size_t index=i-1;if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[index],Value::Number(static_cast<double>(index)),Value::FromObject(object)})))return Value::Number(static_cast<double>(index));}return Value::Number(-1);});
            if(key==L"some")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Bool(false);for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return Value::Bool(true);return Value::Bool(false);});
            if(key==L"at")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto size=static_cast<long long>(object->items.size());auto index=args.empty()?0ll:static_cast<long long>(r.Number(args[0]));if(index<0)index+=size;return index>=0&&index<size?object->items[static_cast<size_t>(index)]:Value::Undefined();});
            if(key==L"fill")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto size=static_cast<long long>(object->items.size());auto index=[&](size_t argument,long long fallback){if(argument>=args.size())return fallback;auto value=static_cast<long long>(r.Number(args[argument]));return value<0?std::max(0ll,size+value):std::min(size,value);};const auto start=index(1,0),end=index(2,size);const auto value=args.empty()?Value::Undefined():r.Deref(args[0]);for(auto position=start;position<end;++position)object->items[static_cast<size_t>(position)]=value;return Value::FromObject(object);});
            if(key==L"sort")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto compare=args.empty()?Value::Undefined():r.Deref(args[0]);std::stable_sort(object->items.begin(),object->items.end(),[&](const Value& left,const Value& right){if(r.IsCallable(compare)){const auto result=r.Number(r.Call(compare,Value::Undefined(),{left,right}));return std::isnan(result)?false:result<0;}return r.String(left)<r.String(right);});return Value::FromObject(object);});
            if(key==L"slice")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto size=static_cast<long long>(object->items.size());
                auto offset=[&](size_t index,long long fallback){
                    if(index>=args.size())return fallback;
                    const double number=r.Number(args[index]);
                    if(std::isnan(number))return 0ll;
                    const auto integer=static_cast<long long>(number);
                    return integer<0?std::max(0ll,size+integer):std::min(size,integer);
                };
                const auto begin=offset(0,0),end=offset(1,size);
                if(end<=begin)return r.ArrayValue({});
                return r.ArrayValue(std::vector<Value>(
                    object->items.begin()+static_cast<std::ptrdiff_t>(begin),
                    object->items.begin()+static_cast<std::ptrdiff_t>(end)));
            });
            if(key==L"splice")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto size=static_cast<std::ptrdiff_t>(object->items.size());
                auto boundedIndex=[&](const Value& value){const double number=r.Number(value);
                    if(std::isnan(number))return std::ptrdiff_t{0};
                    return static_cast<std::ptrdiff_t>(std::max(-static_cast<double>(size),std::min(static_cast<double>(size),number)));
                };
                std::ptrdiff_t start=args.empty()?0:boundedIndex(args[0]);
                if(start<0)start=std::max(std::ptrdiff_t{0},size+start);else start=std::min(start,size);
                std::ptrdiff_t count=args.size()>1?boundedIndex(args[1]):size-start;
                count=std::max(std::ptrdiff_t{0},std::min(count,size-start));auto first=object->items.begin()+start;
                std::vector<Value> removed(first,first+count);object->items.erase(first,first+count);
                if(args.size()>2)object->items.insert(object->items.begin()+start,args.begin()+2,args.end());
                return r.ArrayValue(removed);
            });
            if(key==L"forEach"||key==L"filter"||key==L"every"||key==L"map")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                if(args.empty())return key==L"every"?Value::Bool(true):(key==L"filter"||key==L"map"?r.ArrayValue({}):Value::Undefined());std::vector<Value> output;if(key==L"filter"||key==L"map")output.reserve(object->items.size());
                std::vector<Value> callbackArgs(3);callbackArgs[2]=Value::FromObject(object);
                for(size_t i=0;i<object->items.size();++i){callbackArgs[0]=object->items[i];callbackArgs[1]=Value::Number(static_cast<double>(i));auto result=r.Call(args[0],Value::Undefined(),callbackArgs);if(key==L"filter"&&r.Truth(result))output.push_back(object->items[i]);if(key==L"map")output.push_back(r.Deref(std::move(result)));if(key==L"every"&&!r.Truth(result))return Value::Bool(false);}
                if(key==L"filter"||key==L"map")return r.ArrayValue(output);if(key==L"every")return Value::Bool(true);return Value::Undefined();});
            if(key==L"join")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto separator=args.empty()?L",":r.String(args[0]);
                size_t capacity=separator.size()*(object->items.empty()?0:object->items.size()-1);
                bool stringsOnly=true;
                for(const auto& item:object->items){
                    const auto value=r.Deref(item);
                    if(value.type==Value::Type::String)capacity+=value.string.size();
                    else if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null){stringsOnly=false;break;}
                }
                std::wstring out;
                if(stringsOnly)out.reserve(capacity);
                for(size_t i=0;i<object->items.size();++i){
                    if(i)out+=separator;
                    const auto value=r.Deref(object->items[i]);
                    if(value.type==Value::Type::String)out+=value.string;
                    else if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null)out+=r.String(value);
                }
                return Value::String(std::move(out));
            });
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<object->items.size())return object->items[index];
        }
        if(object->kind==ObjectKind::RegExp&&(key==L"test"||key==L"exec"))return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto input=a.empty()?L"":r.String(a[0]),pattern=r.String(object->props[L"$pattern"]),flags=r.String(object->props[L"$flags"]);
            if(!r.RegularExpressionMayMatch(pattern,flags,input))return key==L"test"?Value::Bool(false):Value::Null();
            if(const auto expression=r.RegularExpression(pattern,flags)){std::wsmatch match;const bool found=std::regex_search(input,match,*expression);if(key==L"test")return Value::Bool(found);if(!found)return Value::Null();std::vector<Value> captures;for(const auto& capture:match)captures.push_back(Value::String(capture.str()));return r.ArrayValue(captures);}return key==L"test"?Value::Bool(false):Value::Null();
        });
        if(object->kind==ObjectKind::ObjectConstructor){
            if(key==L"hasOwn")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.size()<2)return Value::Bool(false);const auto source=r.Deref(a[0]);if(source.type!=Value::Type::Object||!source.object)return Value::Bool(false);const auto name=r.String(a[1]);if(source.object->kind==ObjectKind::Array){size_t index=0;if(name==L"length")return Value::Bool(true);if(TryParseDecimalIndex(name,index))return Value::Bool(index<source.object->items.size());}return Value::Bool(source.object->props.count(name)!=0);});
            if(key==L"create")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ObjectValue(ObjectKind::Plain);});
            if(key==L"freeze")return Native([](RuntimeCore&,const Value&,const std::vector<Value>& a){return a.empty()?Value::Undefined():a[0];});
            if(key==L"fromEntries")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto result=r.ObjectValue(ObjectKind::Plain);
                if(!a.empty()){auto entries=r.Deref(a[0]);
                    if(entries.type==Value::Type::Object&&entries.object&&entries.object->kind==ObjectKind::Array)
                        for(const auto& item:entries.object->items){auto pair=r.Deref(item);
                            if(pair.type==Value::Type::Object&&pair.object&&pair.object->kind==ObjectKind::Array&&pair.object->items.size()>1)
                                result.object->props[r.String(pair.object->items[0])]=r.Deref(pair.object->items[1]);
                        }
                }
                return result;
            });
            if(key==L"entries"||key==L"values"||key==L"keys")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<Value> result;if(a.empty())return r.ArrayValue(result);
                auto source=r.Deref(a[0]);if(source.type!=Value::Type::Object||!source.object)return r.ArrayValue(result);
                for(const auto& property:source.object->props){
                    auto name=Value::String(property.first),value=r.Deref(property.second);
                    result.push_back(key==L"keys"?name:key==L"values"?value:r.ArrayValue({name,value}));
                }
                return r.ArrayValue(result);
            });
        }
        if(object->kind==ObjectKind::Map||object->kind==ObjectKind::Set){
            const bool map=object->kind==ObjectKind::Map;
            if(key==L"size")return Value::Number(static_cast<double>(object->entries.size()));
            if(key==L"clear")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->entries.clear();return Value::Undefined();});
            if(key==L"values")return Native([object,map](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> values;values.reserve(object->entries.size());for(const auto& entry:object->entries)values.push_back(map?entry.second:entry.first);return r.ArrayValue(values);});
            if(key==L"has"||key==L"get"||key==L"set"||key==L"add"||key==L"delete")return Native([object,key,map](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto needle=args.empty()?Value::Undefined():r.Deref(args[0]);
                auto found=std::find_if(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.EqualValues(entry.first,needle);});
                if(key==L"has")return Value::Bool(found!=object->entries.end());
                if(key==L"get")return found==object->entries.end()?Value::Undefined():found->second;
                if(key==L"delete"){if(found==object->entries.end())return Value::Bool(false);object->entries.erase(found);return Value::Bool(true);}
                const auto value=map?(args.size()>1?r.Deref(args[1]):Value::Undefined()):needle;
                if(found==object->entries.end())object->entries.push_back({needle,value});else found->second=value;
                return Value::FromObject(object);
            });
        }
        if(object->kind==ObjectKind::CanvasGradient){
            if(key==L"addColorStop")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->canvasGradient||a.size()<2)return Value::Undefined();
                const double rawOffset=r.Number(a[0]);
                if(!std::isfinite(rawOffset)||rawOffset<0||rawOffset>1)return Value::Undefined();
                object->canvasGradient->stops.push_back(
                    {static_cast<float>(rawOffset),r.String(a[1])});
                std::stable_sort(object->canvasGradient->stops.begin(),
                    object->canvasGradient->stops.end(),
                    [](const CanvasGradientStop& left,const CanvasGradientStop& right){
                        return left.offset<right.offset;
                    });
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::CanvasContext2D){
            const auto surface=EnsureCanvas(object->node);if(!surface)return Value::Undefined();
            if(key==L"canvas")return NodeValue(object->node);
            if(key==L"fillStyle"||key==L"strokeStyle"){
                const auto& paint=key==L"fillStyle"?surface->state.fillStyle:surface->state.strokeStyle;
                if(!paint.gradient)return Value::String(paint.color);
                auto gradient=CreateObject(ObjectKind::CanvasGradient);
                gradient->canvasGradient=paint.gradient;return Value::FromObject(gradient);
            }
            if(key==L"lineWidth")return Value::Number(surface->state.lineWidth);
            if(key==L"font")return Value::String(surface->state.font);
            if(key==L"textAlign")return Value::String(surface->state.textAlign);
            if(key==L"textBaseline")return Value::String(surface->state.textBaseline);
            if(key==L"save")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->stateStack.push_back(value->state);
                return Value::Undefined();
            });
            if(key==L"restore")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node);value&&!value->stateStack.empty()){
                    value->state=value->stateStack.back();value->stateStack.pop_back();
                }
                return Value::Undefined();
            });
            if(key==L"setTransform"||key==L"transform")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<6)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasTransform transform{static_cast<float>(r.Number(a[0])),static_cast<float>(r.Number(a[1])),
                    static_cast<float>(r.Number(a[2])),static_cast<float>(r.Number(a[3])),
                    static_cast<float>(r.Number(a[4])),static_cast<float>(r.Number(a[5]))};
                if(!std::isfinite(transform.a)||!std::isfinite(transform.b)||!std::isfinite(transform.c)||
                   !std::isfinite(transform.d)||!std::isfinite(transform.e)||!std::isfinite(transform.f))return Value::Undefined();
                if(key==L"setTransform")value->state.transform=transform;
                else value->state.transform=MultiplyCanvasTransforms(transform,value->state.transform);
                return Value::Undefined();
            });
            if(key==L"resetTransform")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->state.transform={};return Value::Undefined();
            });
            if(key==L"translate"||key==L"scale"||key==L"rotate")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();CanvasTransform transform;
                if(key==L"translate"){
                    transform.e=static_cast<float>(a.empty()?0:r.Number(a[0]));
                    transform.f=static_cast<float>(a.size()>1?r.Number(a[1]):0);
                }else if(key==L"scale"){
                    transform.a=static_cast<float>(a.empty()?1:r.Number(a[0]));
                    transform.d=static_cast<float>(a.size()>1?r.Number(a[1]):transform.a);
                }else{
                    const float angle=static_cast<float>(a.empty()?0:r.Number(a[0]));
                    transform.a=std::cos(angle);transform.b=std::sin(angle);
                    transform.c=-std::sin(angle);transform.d=std::cos(angle);
                }
                value->state.transform=MultiplyCanvasTransforms(transform,value->state.transform);
                return Value::Undefined();
            });
            if(key==L"beginPath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->currentPath.clear();return Value::Undefined();
            });
            if(key==L"closePath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->currentPath.push_back({CanvasPathVerb::Close});
                return Value::Undefined();
            });
            if(key==L"moveTo"||key==L"lineTo")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                const float x=static_cast<float>(r.Number(a[0])),y=static_cast<float>(r.Number(a[1]));
                if(!std::isfinite(x)||!std::isfinite(y))return Value::Undefined();CanvasPathSegment segment;
                segment.verb=key==L"moveTo"?CanvasPathVerb::MoveTo:CanvasPathVerb::LineTo;
                segment.point=TransformCanvasPoint(value->state.transform,x,y);value->currentPath.push_back(segment);
                return Value::Undefined();
            });
            if(key==L"arc")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<5)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasPathSegment segment;segment.verb=CanvasPathVerb::Arc;
                segment.centerX=static_cast<float>(r.Number(a[0]));segment.centerY=static_cast<float>(r.Number(a[1]));
                segment.radius=static_cast<float>(r.Number(a[2]));segment.startAngle=static_cast<float>(r.Number(a[3]));
                segment.endAngle=static_cast<float>(r.Number(a[4]));segment.counterClockwise=a.size()>5&&r.Truth(a[5]);
                segment.transform=value->state.transform;
                if(segment.radius<0||!std::isfinite(segment.centerX)||!std::isfinite(segment.centerY)||
                   !std::isfinite(segment.radius)||!std::isfinite(segment.startAngle)||!std::isfinite(segment.endAngle))
                    return Value::Undefined();
                value->currentPath.push_back(segment);return Value::Undefined();
            });
            if(key==L"fill"||key==L"stroke")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto value=r.EnsureCanvas(object->node);if(!value||value->currentPath.empty())return Value::Undefined();
                CanvasDrawCommand command;command.kind=key==L"fill"?CanvasCommandKind::FillPath:CanvasCommandKind::StrokePath;
                command.state=SnapshotCanvasState(value->state);command.path=value->currentPath;
                value->commands.push_back(std::move(command));r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);
                return Value::Undefined();
            });
            if(key==L"fillRect"||key==L"clearRect")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<4)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasDrawCommand command;command.kind=key==L"fillRect"?CanvasCommandKind::FillRect:CanvasCommandKind::ClearRect;
                command.state=SnapshotCanvasState(value->state);command.x=static_cast<float>(r.Number(a[0]));
                command.y=static_cast<float>(r.Number(a[1]));command.width=static_cast<float>(r.Number(a[2]));
                command.height=static_cast<float>(r.Number(a[3]));
                const auto& t=value->state.transform;
                const bool identity=t.a==1&&t.b==0&&t.c==0&&t.d==1&&t.e==0&&t.f==0;
                if(command.kind==CanvasCommandKind::ClearRect&&identity&&command.x<=0&&command.y<=0&&
                   command.x+command.width>=value->width&&command.y+command.height>=value->height)
                    value->commands.clear();
                else value->commands.push_back(std::move(command));
                r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);return Value::Undefined();
            });
            if(key==L"createLinearGradient")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<4)return Value::Null();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Null();
                const auto first=TransformCanvasPoint(value->state.transform,static_cast<float>(r.Number(a[0])),static_cast<float>(r.Number(a[1])));
                const auto second=TransformCanvasPoint(value->state.transform,static_cast<float>(r.Number(a[2])),static_cast<float>(r.Number(a[3])));
                auto gradient=r.CreateObject(ObjectKind::CanvasGradient);
                gradient->canvasGradient=std::make_shared<CanvasGradient>();gradient->canvasGradient->x0=first.x;
                gradient->canvasGradient->y0=first.y;gradient->canvasGradient->x1=second.x;gradient->canvasGradient->y1=second.y;
                return Value::FromObject(gradient);
            });
            if(key==L"fillText")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<3)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasDrawCommand command;command.kind=CanvasCommandKind::FillText;command.state=SnapshotCanvasState(value->state);
                command.text=r.String(a[0]);command.x=static_cast<float>(r.Number(a[1]));command.y=static_cast<float>(r.Number(a[2]));
                value->commands.push_back(std::move(command));r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Document){
            if(key==L"documentElement")return NodeValue(document.QuerySelector(L"html"));
            if(key==L"body")return NodeValue(document.Body());
            if(key==L"activeElement"){
                std::shared_ptr<Node> active;
                std::function<void(const std::shared_ptr<Node>&)> find=[&](const std::shared_ptr<Node>& node){
                    if(!node||active)return;if(node->focused){active=node;return;}
                    for(const auto& child:node->children)find(child);
                };
                find(document.Root());return NodeValue(active);
            }
            if(key==L"getElementById")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();return r.NodeValue(r.document.GetElementById(a.empty()?L"":r.String(a[0])));});
            if(key==L"getElementsByName")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<Value> out;for(auto& n:r.document.GetElementsByName(a.empty()?L"":r.String(a[0])))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"querySelector"||key==L"querySelectorAll")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();auto selector=a.empty()?L"":r.String(a[0]);if(key==L"querySelector")return r.NodeValue(r.document.QuerySelector(selector));std::vector<Value> out;for(auto& n:r.document.QuerySelectorAll(selector))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"createElement")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(r.document.CreateElement(a.empty()?L"div":r.String(a[0])));});
            if(key==L"createTextNode")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto node=std::make_shared<Node>();node->type=NodeType::Text;node->tag=L"#text";node->text=a.empty()?L"":r.String(a[0]);node->ownerDocument=&r.document;return r.NodeValue(node);});
            if(key==L"createDocumentFragment")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){auto node=std::make_shared<Node>();node->type=NodeType::Element;node->tag=L"#document-fragment";node->ownerDocument=&r.document;return r.NodeValue(node);});
            if(key==L"createTreeWalker")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();const auto root=r.Deref(a[0]);
                if(root.type!=Value::Type::Object||!root.object||root.object->kind!=ObjectKind::Node)return Value::Null();
                auto walker=r.CreateObject(ObjectKind::TreeWalker);walker->node=root.object->node;
                walker->props[L"$current"]=r.NodeValue(root.object->node);
                walker->props[L"$whatToShow"]=Value::Number(a.size()>1?r.Number(a[1]):0xffffffffu);
                return Value::FromObject(walker);
            });
            if(key==L"createRange")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto root=r.document.Root();return r.RangeValue(root,0,root,0);
            });
            if(key==L"execCommand")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto command=a.empty()?L"":r.String(a[0]);
                const auto value=a.size()>2?r.String(a[2]):L"";
                return Value::Bool(r.editingCommands.Execute(command,value));
            });
            if(key==L"queryCommandState")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return Value::Bool(!a.empty()&&r.editingCommands.QueryState(r.String(a[0])));
            });
            if(key==L"queryCommandSupported")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return Value::Bool(!a.empty()&&
                    EditingCommandExecutor::IsSupported(r.String(a[0])));
            });
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.documentListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.documentListeners,a);return Value::Undefined();});
        }
        if(object->kind==ObjectKind::Range){
            if(key==L"startContainer")return NodeValue(object->rangeStart);
            if(key==L"endContainer")return NodeValue(object->rangeEnd);
            if(key==L"startOffset")return Value::Number(static_cast<double>(object->rangeStartOffset));
            if(key==L"endOffset")return Value::Number(static_cast<double>(object->rangeEndOffset));
            if(key==L"collapsed")return Value::Bool(object->rangeStart==object->rangeEnd&&object->rangeStartOffset==object->rangeEndOffset);
            if(key==L"commonAncestorContainer")return NodeValue(CommonAncestor(object->rangeStart,object->rangeEnd));
            if(key==L"cloneRange")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                return r.RangeValue(object->rangeStart,object->rangeStartOffset,object->rangeEnd,object->rangeEndOffset);
            });
            if(key==L"selectNodeContents")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Undefined();
                object->rangeStart=object->rangeEnd=value.object->node;object->rangeStartOffset=0;
                object->rangeEndOffset=value.object->node->type==NodeType::Text?value.object->node->text.size():value.object->node->children.size();
                return Value::Undefined();
            });
            if(key==L"deleteContents")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){r.DeleteRangeContents(object);return Value::Undefined();});
            if(key==L"insertNode")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)r.InsertRangeNode(object,value.object->node);
                return Value::Undefined();
            });
            if(key==L"setStart"||key==L"setEnd")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Undefined();
                const auto offset=static_cast<size_t>(std::max(0.0,r.Number(a[1])));
                if(key==L"setStart"){object->rangeStart=value.object->node;object->rangeStartOffset=offset;}
                else{object->rangeEnd=value.object->node;object->rangeEndOffset=offset;}
                return Value::Undefined();
            });
            if(key==L"setStartAfter"||key==L"setEndAfter")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Undefined();
                const auto parent=value.object->node->parent.lock();if(!parent)return Value::Undefined();
                const auto found=std::find(parent->children.begin(),parent->children.end(),value.object->node);
                const size_t offset=found==parent->children.end()?parent->children.size():static_cast<size_t>(found-parent->children.begin())+1;
                if(key==L"setStartAfter"){object->rangeStart=parent;object->rangeStartOffset=offset;}
                else{object->rangeEnd=parent;object->rangeEndOffset=offset;}
                return Value::Undefined();
            });
            if(key==L"collapse")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty()||r.Truth(a[0])){object->rangeEnd=object->rangeStart;object->rangeEndOffset=object->rangeStartOffset;}
                else{object->rangeStart=object->rangeEnd;object->rangeStartOffset=object->rangeEndOffset;}
                return Value::Undefined();
            });
            if(key==L"toString")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(RangeText(object));});
        }
        if(object->kind==ObjectKind::TreeWalker){
            if(key==L"root")return NodeValue(object->node);
            if(key==L"currentNode")return object->props[L"$current"];
            if(key==L"nextNode")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                auto currentValue=r.Deref(object->props[L"$current"]);auto current=currentValue.type==Value::Type::Object&&currentValue.object?currentValue.object->node:object->node;
                const auto mask=static_cast<unsigned>(r.Number(object->props[L"$whatToShow"]));
                for(auto next=NextNodeWithin(current,object->node);next;next=NextNodeWithin(next,object->node)){
                    const unsigned flag=next->type==NodeType::Element?1u:next->type==NodeType::Text?4u:0x100u;
                    if(mask==0xffffffffu||(mask&flag)){object->props[L"$current"]=r.NodeValue(next);return r.NodeValue(next);}
                }
                return Value::Null();
            });
        }
        if(object->kind==ObjectKind::Selection){
            const auto stored=object->props.find(L"$range");
            const auto range=stored!=object->props.end()&&stored->second.type==Value::Type::Object?stored->second.object:std::shared_ptr<Object>{};
            if(key==L"rangeCount")return Value::Number(range?1:0);
            if(key==L"anchorNode")return NodeValue(range?range->rangeStart:std::shared_ptr<Node>{});
            if(key==L"focusNode")return NodeValue(range?range->rangeEnd:std::shared_ptr<Node>{});
            if(key==L"anchorOffset")return Value::Number(static_cast<double>(range?range->rangeStartOffset:0));
            if(key==L"focusOffset")return Value::Number(static_cast<double>(range?range->rangeEndOffset:0));
            if(key==L"isCollapsed")return Value::Bool(!range||(range->rangeStart==range->rangeEnd&&range->rangeStartOffset==range->rangeEndOffset));
            if(key==L"getRangeAt")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto found=object->props.find(L"$range");
                if(a.empty()||r.Number(a[0])!=0||found==object->props.end())return Value::Undefined();return found->second;
            });
            if(key==L"removeAllRanges")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props.erase(L"$range");return Value::Undefined();});
            if(key==L"addRange")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Range)return Value::Undefined();
                object->props[L"$range"]=value;
                if(r.domSelectionSetter){JavaScriptRuntime::DomSelection selection;
                    selection.anchorNode=value.object->rangeStart;selection.anchorOffset=value.object->rangeStartOffset;
                    selection.focusNode=value.object->rangeEnd;selection.focusOffset=value.object->rangeEndOffset;
                    r.domSelectionSetter(selection);
                }
                return Value::Undefined();
            });
            if(key==L"toString")return Native([range](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(RangeText(range));});
        }
        if(object->kind==ObjectKind::Node){
            auto node=object->node;if(!node)return Value::Undefined();
            if(key==L"contentWindow"&&node->tag==L"iframe")return FrameWindowValue(node);
            if(node->tag==L"img"&&(key==L"naturalWidth"||key==L"naturalHeight"))
                return Value::Number(node->image?
                    (key==L"naturalWidth"?node->image->width:node->image->height):0);
            if(node->tag==L"img"&&key==L"complete")return Value::Bool(node->imageComplete);
            if(node->tag==L"img"&&(key==L"width"||key==L"height")){
                const auto authored=Trim(node->Attribute(key));
                double parsed=0;
                if(TryParseDouble(authored,parsed)&&std::isfinite(parsed))
                    return Value::Number(std::max(0.0,parsed));
                if(node->image)return Value::Number(
                    key==L"width"?node->image->width:node->image->height);
                return Value::Number(0);
            }
            if(node->tag==L"canvas"&&(key==L"width"||key==L"height"))
                return Value::Number(CanvasDimension(node,key.c_str(),key==L"width"?300:150));
            if(node->tag==L"canvas"&&key==L"getContext")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty()||ToLower(Trim(r.String(a[0])))!=L"2d")return Value::Null();
                return r.CanvasContextValue(node);
            });
            if(key==L"id")return Value::String(node->Attribute(L"id"));
            if(key==L"className")return Value::String(node->Attribute(L"class"));
            if(key==L"value")return Value::String(node->Attribute(L"value"));
            if(key==L"nodeType")return Value::Number(IsDocumentFragment(node)?11:node->type==NodeType::Element?1:node->type==NodeType::Text?3:9);
            if(key==L"length"&&node->type==NodeType::Text)return Value::Number(static_cast<double>(node->text.size()));
            if(key==L"nodeValue")return node->type==NodeType::Text?Value::String(node->text):Value::Null();
            if(key==L"tagName"){auto tag=node->type==NodeType::Element?node->tag:L"";std::transform(tag.begin(),tag.end(),tag.begin(),[](wchar_t character){return static_cast<wchar_t>(std::towupper(character));});return Value::String(std::move(tag));}
            if(key==L"selectionStart"||key==L"selectionEnd"){
                size_t start=node->selectionStart,end=node->selectionEnd;
                if(selectionProvider)selectionProvider(node,start,end);
                return Value::Number(static_cast<double>(key==L"selectionStart"?start:end));
            }
            if(key==L"selectionDirection")return Value::String(node->selectionDirection);
            if(key==L"files"&&node->tag==L"input"&&ToLower(node->Attribute(L"type"))==L"file"){
                return FileListValue(node->files);
            }
            if(key==L"checked")return Value::Bool(node->checked);
            if(key==L"indeterminate")return Value::Bool(node->indeterminate);
            if(key==L"disabled")return Value::Bool(node->disabled);
            if(key==L"required")return Value::Bool(node->attributes.count(L"required")!=0);
            if(key==L"hidden")return Value::Bool(node->attributes.count(L"hidden")!=0);
            if(key==L"open")return Value::Bool(node->attributes.count(L"open")!=0);
            if(key==L"defer")return Value::Bool(node->attributes.count(L"defer")!=0);
            if(key==L"contentEditable")return Value::String(node->Attribute(L"contenteditable"));
            if(key==L"title"||key==L"type"||key==L"lang"||key==L"src"||key==L"href"||
               key==L"name"||key==L"placeholder"||key==L"rel"||key==L"alt"||
               key==L"draggable"||key==L"colSpan"||key==L"rowSpan"||key==L"returnValue")
                return key==L"draggable"?Value::Bool(node->Attribute(L"draggable")==L"true"):
                    Value::String(node->Attribute(key==L"colSpan"?L"colspan":
                        (key==L"rowSpan"?L"rowspan":ToLower(key))));
            if(key==L"scrollTop")return Value::Number(node->scrollTop);
            if(key==L"scrollLeft")return Value::Number(node->scrollLeft);
            if(key==L"clientHeight"||key==L"clientWidth"||key==L"scrollHeight"||key==L"scrollWidth"){
                const auto g=geometryProvider?geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                return Value::Number(key==L"clientHeight"?g.clientHeight:key==L"clientWidth"?g.clientWidth:key==L"scrollWidth"?g.scrollWidth:g.scrollHeight);
            }
            if(key==L"getBoundingClientRect")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto g=r.geometryProvider?r.geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                auto rect=r.ObjectValue(ObjectKind::Plain);
                rect.object->props[L"x"]=rect.object->props[L"left"]=Value::Number(g.x);
                rect.object->props[L"y"]=rect.object->props[L"top"]=Value::Number(g.y);
                rect.object->props[L"width"]=Value::Number(g.width);
                rect.object->props[L"height"]=Value::Number(g.height);
                rect.object->props[L"right"]=Value::Number(g.x+g.width);
                rect.object->props[L"bottom"]=Value::Number(g.y+g.height);
                return rect;
            });
            if(key==L"innerText"||key==L"textContent")return Value::String(node->InnerText());
            if(key==L"innerHTML")return Value::String(InnerHtml(node));
            if(key==L"content"&&node->tag==L"template")return NodeValue(node);
            if(key==L"children"){
                std::vector<Value> out;for(const auto& child:node->children)if(child->type==NodeType::Element)out.push_back(NodeValue(child));return ArrayValue(out);
            }
            if(key==L"childNodes"){
                std::vector<Value> out;for(const auto& child:node->children)out.push_back(NodeValue(child));return ArrayValue(out);
            }
            if(key==L"childElementCount"){
                size_t count=0;for(const auto& child:node->children)if(child->type==NodeType::Element)++count;return Value::Number(static_cast<double>(count));
            }
            if(key==L"firstElementChild"||key==L"lastElementChild"){
                if(key==L"firstElementChild"){for(const auto& child:node->children)if(child->type==NodeType::Element)return NodeValue(child);}
                else{for(auto child=node->children.rbegin();child!=node->children.rend();++child)if((*child)->type==NodeType::Element)return NodeValue(*child);}
                return Value::Null();
            }
            if(key==L"classList"){auto o=CreateObject(ObjectKind::ClassList);o->node=node;return Value::FromObject(o);}
            if(key==L"style"){auto o=CreateObject(ObjectKind::Style);o->node=node;return Value::FromObject(o);}
            if(key==L"dataset"){auto o=CreateObject(ObjectKind::Dataset);o->node=node;return Value::FromObject(o);}
            if(key==L"cells"){std::vector<Value> out;for(auto& c:node->children)if(c->tag==L"td"||c->tag==L"th")out.push_back(NodeValue(c));return ArrayValue(out);}
            if(key==L"tBodies"){std::vector<Value> out;for(auto& child:node->children)if(child->tag==L"tbody")out.push_back(NodeValue(child));return ArrayValue(out);}
            if(key==L"rows"){
                std::vector<Value> out;std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& current){
                    for(const auto& child:current->children){if(child->tag==L"tr")out.push_back(NodeValue(child));else if(child->tag==L"thead"||child->tag==L"tbody"||child->tag==L"tfoot")visit(child);}
                };visit(node);return ArrayValue(out);
            }
            if(key==L"parentNode")return NodeValue(node->parent.lock());
            if(key==L"parentElement"){const auto parent=node->parent.lock();return NodeValue(parent&&parent->type==NodeType::Element?parent:std::shared_ptr<Node>{});}
            if(key==L"firstChild"||key==L"lastChild")return NodeValue(node->children.empty()?std::shared_ptr<Node>{}:(key==L"firstChild"?node->children.front():node->children.back()));
            if(key==L"nextSibling"||key==L"previousSibling"){
                const auto parent=node->parent.lock();if(!parent)return Value::Null();
                const auto found=std::find(parent->children.begin(),parent->children.end(),node);if(found==parent->children.end())return Value::Null();
                if(key==L"nextSibling")return NodeValue(found+1==parent->children.end()?std::shared_ptr<Node>{}:*(found+1));
                return NodeValue(found==parent->children.begin()?std::shared_ptr<Node>{}:*(found-1));
            }
            if(key==L"nextElementSibling"){
                auto parent=node->parent.lock();if(!parent)return Value::Null();bool found=false;for(const auto& sibling:parent->children){if(sibling==node){found=true;continue;}if(found&&sibling->type==NodeType::Element)return NodeValue(sibling);}return Value::Null();
            }
            if(key==L"appendChild")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                auto child=value.object->node;auto old=child->parent.lock();
                bool indexOk=r.document.UnindexSubtree(child);
                if(old){
                    old->children.erase(std::remove(old->children.begin(),old->children.end(),child),old->children.end());
                }
                child->parent=node;node->children.push_back(child);
                indexOk=r.document.IndexSubtree(child)&&indexOk;
                if(old)r.Mutated(old,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.ExecuteConnectedScripts(child);return value;
            });
            if(key==L"insertBefore")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();auto value=r.Deref(a[0]);if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                auto child=value.object->node;auto old=child->parent.lock();bool indexOk=r.document.UnindexSubtree(child);
                if(old)old->children.erase(std::remove(old->children.begin(),old->children.end(),child),old->children.end());
                auto position=node->children.end();if(a.size()>1){auto before=r.Deref(a[1]);if(before.type==Value::Type::Object&&before.object&&before.object->kind==ObjectKind::Node)position=std::find(node->children.begin(),node->children.end(),before.object->node);}
                child->parent=node;node->children.insert(position,child);indexOk=r.document.IndexSubtree(child)&&indexOk;
                if(old)r.Mutated(old,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.ExecuteConnectedScripts(child);return value;
            });
            if(key==L"append"||key==L"prepend"||key==L"replaceChildren")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                bool indexOk=true;
                if(key==L"replaceChildren"){
                    for(auto& child:node->children)indexOk=r.document.UnindexSubtree(child)&&indexOk;
                    for(auto& child:node->children)child->parent.reset();node->children.clear();
                }
                size_t insertion=key==L"prepend"?0:node->children.size();
                for(const auto& input:a){
                    auto value=r.Deref(input);std::shared_ptr<Node> child;
                    if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)child=value.object->node;
                    else{child=std::make_shared<Node>();child->type=NodeType::Text;child->tag=L"#text";child->text=r.String(value);}
                    if(!child)continue;std::vector<std::shared_ptr<Node>> inserted;
                    if(IsDocumentFragment(child)){inserted=child->children;child->children.clear();}
                    else inserted.push_back(child);
                    for(auto& item:inserted){auto old=item->parent.lock();indexOk=r.document.UnindexSubtree(item)&&indexOk;
                        if(old){old->children.erase(std::remove(old->children.begin(),old->children.end(),item),old->children.end());r.Mutated(old,JavaScriptRuntime::MutationKind::Tree);}
                        item->parent=node;node->children.insert(node->children.begin()+static_cast<std::ptrdiff_t>(insertion++),item);indexOk=r.document.IndexSubtree(item)&&indexOk;
                        r.ExecuteConnectedScripts(item);
                    }
                }
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"removeChild")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                const auto child=value.object->node;const auto position=std::find(node->children.begin(),node->children.end(),child);
                if(position==node->children.end())return Value::Null();
                const bool indexOk=r.document.UnindexSubtree(child);node->children.erase(position);child->parent.reset();
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return value;
            });
            if(key==L"setAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()>1){
                    const auto name=ToLower(r.String(a[0]));
                    const auto oldId=name==L"id"?node->Attribute(L"id"):L"";
                    node->SetAttribute(name,r.String(a[1]));
                    if(node->tag==L"dialog"&&name==L"open")node->modal=false;
                    if(name==L"src")ResetImageSourceState(node);
                    if(node->tag==L"canvas"&&(name==L"width"||name==L"height"))r.ResetCanvas(node);
                    const bool indexOk=name!=L"id"||
                        r.document.UpdateElementId(node,oldId,node->Attribute(L"id"));
                    const auto kind=AttributeRequiresLayout(name)?JavaScriptRuntime::MutationKind::Layout:JavaScriptRuntime::MutationKind::Style;
                    r.Mutated(node,kind,!indexOk,name==L"aria-live");
                }
                return Value::Undefined();
            });
            if(key==L"getAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::String(a.empty()?L"":node->Attribute(r.String(a[0])));});
            if(key==L"hasAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&node->attributes.count(ToLower(r.String(a[0])))!=0);});
            if(key==L"removeAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!a.empty()){
                    const auto name=ToLower(r.String(a[0]));
                    const auto oldId=name==L"id"?node->Attribute(L"id"):L"";
                    node->RemoveAttribute(name);
                    if(node->tag==L"dialog"&&name==L"open")node->modal=false;
                    if(name==L"src")ResetImageSourceState(node);
                    if(node->tag==L"canvas"&&(name==L"width"||name==L"height"))r.ResetCanvas(node);
                    const bool indexOk=name!=L"id"||r.document.UpdateElementId(node,oldId,L"");
                    const auto kind=AttributeRequiresLayout(name)?JavaScriptRuntime::MutationKind::Layout:JavaScriptRuntime::MutationKind::Style;
                    r.Mutated(node,kind,!indexOk,name==L"aria-live");
                }
                return Value::Undefined();
            });
            if(key==L"toggleAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto name=ToLower(r.String(a[0]));
                const bool present=node->attributes.count(name)!=0;const bool enabled=a.size()>1?r.Truth(a[1]):!present;
                if(enabled&&!present)node->SetAttribute(name,L"");else if(!enabled&&present)node->RemoveAttribute(name);
                if(node->tag==L"dialog"&&name==L"open")node->modal=false;
                r.Mutated(node,JavaScriptRuntime::MutationKind::Style);return Value::Bool(enabled);
            });
            if(key==L"remove")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(auto parent=node->parent.lock()){const bool indexOk=r.document.UnindexSubtree(node);parent->children.erase(std::remove(parent->children.begin(),parent->children.end(),node),parent->children.end());node->parent.reset();r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);}return Value::Undefined();});
            if(key==L"replaceWith")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto parent=node->parent.lock();if(!parent)return Value::Undefined();auto position=std::find(parent->children.begin(),parent->children.end(),node);if(position==parent->children.end())return Value::Undefined();
                bool indexOk=r.document.UnindexSubtree(node);const auto offset=static_cast<size_t>(position-parent->children.begin());parent->children.erase(position);node->parent.reset();size_t insert=offset;
                for(const auto& input:a){auto value=r.Deref(input);std::shared_ptr<Node> replacement;if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)replacement=value.object->node;else{replacement=std::make_shared<Node>();replacement->type=NodeType::Text;replacement->tag=L"#text";replacement->text=r.String(value);}std::vector<std::shared_ptr<Node>> replacements;if(IsDocumentFragment(replacement)){replacements=replacement->children;replacement->children.clear();}else replacements.push_back(replacement);for(auto& item:replacements){auto old=item->parent.lock();indexOk=r.document.UnindexSubtree(item)&&indexOk;if(old){old->children.erase(std::remove(old->children.begin(),old->children.end(),item),old->children.end());r.Mutated(old,JavaScriptRuntime::MutationKind::Tree);}item->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insert++),item);indexOk=r.document.IndexSubtree(item)&&indexOk;r.ExecuteConnectedScripts(item);}}
                r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"cloneNode")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(CloneDomNode(node,!a.empty()&&r.Truth(a[0])));});
            if(key==L"normalize")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                // normalize() is commonly called after every contenteditable
                // input.  A single existing text node is already normalized;
                // avoid turning that no-op into a full DOM index and layout
                // rebuild. Text-node merges do not affect element indexes.
                if(NormalizeDomNode(node))r.Mutated(node,JavaScriptRuntime::MutationKind::Tree);
                return Value::Undefined();
            });
            if(key==L"focus")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.focusSink)r.focusSink(node);else{node->focused=true;r.Mutated(node,JavaScriptRuntime::MutationKind::Style);}return Value::Undefined();});
            if(key==L"blur")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.focusSink)r.focusSink({});return Value::Undefined();});
            if(key==L"click")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.activationSink)r.activationSink(node);else r.Dispatch(node,L"click");return Value::Undefined();});
            if(key==L"dispatchEvent")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Event)throw JavaScriptException{r.ErrorValue(L"TypeError",L"dispatchEvent requires an Event")};
                JavaScriptRuntime::EventInit init{};init.bubbles=r.Truth(r.GetProperty(value,L"bubbles"));init.cancelable=r.Truth(r.GetProperty(value,L"cancelable"));
                init.data=r.String(r.GetProperty(value,L"data"));init.inputType=r.String(r.GetProperty(value,L"inputType"));
                return Value::Bool(!r.Dispatch(node,r.String(r.GetProperty(value,L"type")),nullptr,init));
            });
            if(key==L"setSelectionRange")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto length=node->Attribute(L"value").size();
                const auto start=std::min(length,static_cast<size_t>(std::max(0.0,a.empty()?0:r.Number(a[0]))));
                const auto end=std::min(length,static_cast<size_t>(std::max(0.0,a.size()>1?r.Number(a[1]):static_cast<double>(start))));
                const auto direction=a.size()>2?ToLower(r.String(a[2])):L"none";
                node->selectionStart=std::min(start,end);node->selectionEnd=std::max(start,end);
                node->selectionDirection=direction==L"forward"||direction==L"backward"?direction:L"none";
                if(r.selectionSetter)r.selectionSetter(node,node->selectionStart,node->selectionEnd);
                return Value::Undefined();
            });
            if(key==L"select")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto length=node->Attribute(L"value").size();node->selectionStart=0;node->selectionEnd=length;node->selectionDirection=L"none";if(r.focusSink)r.focusSink(node);if(r.selectionSetter)r.selectionSetter(node,0,length);return Value::Undefined();});
            if(key==L"setRangeText")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();auto value=node->Attribute(L"value");
                size_t start=a.size()>1?static_cast<size_t>(std::max(0.0,r.Number(a[1]))):node->selectionStart;
                size_t end=a.size()>2?static_cast<size_t>(std::max(0.0,r.Number(a[2]))):node->selectionEnd;
                start=std::min(start,value.size());end=std::min(end,value.size());if(end<start)std::swap(start,end);
                const auto replacement=r.String(a[0]);value.replace(start,end-start,replacement);node->SetAttribute(L"value",value);
                const auto mode=a.size()>3?ToLower(r.String(a[3])):L"preserve";size_t selectionStart=start,selectionEnd=start+replacement.size();
                if(mode==L"start")selectionEnd=selectionStart;else if(mode==L"end")selectionStart=selectionEnd;else if(mode==L"preserve"){
                    const auto delta=static_cast<long long>(replacement.size())-static_cast<long long>(end-start);
                    auto adjust=[&](size_t position){if(position<=start)return position;if(position>=end)return static_cast<size_t>(std::max<long long>(0,static_cast<long long>(position)+delta));return start+replacement.size();};
                    selectionStart=adjust(node->selectionStart);selectionEnd=adjust(node->selectionEnd);
                }
                node->selectionStart=std::min(selectionStart,value.size());node->selectionEnd=std::min(selectionEnd,value.size());
                node->selectionDirection=L"none";
                if(r.selectionSetter)r.selectionSetter(node,node->selectionStart,node->selectionEnd);r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();
            });
            if(key==L"setCustomValidity")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){object->props[L"$customValidity"]=Value::String(a.empty()?L"":r.String(a[0]));return Value::Undefined();});
            if(key==L"reportValidity")return Native([object,node](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto custom=object->props.find(L"$customValidity");const bool customError=custom!=object->props.end()&&!r.String(custom->second).empty();const bool missing=node->attributes.count(L"required")&&!node->disabled&&node->Attribute(L"value").empty();return Value::Bool(!customError&&!missing);});
            if(key==L"reset"&&node->tag==L"form")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                std::function<void(const std::shared_ptr<Node>&)> reset=[&](const std::shared_ptr<Node>& current){for(const auto& child:current->children){if(child->tag==L"input"||child->tag==L"textarea"){const auto type=ToLower(child->Attribute(L"type"));if(type==L"checkbox"||type==L"radio")child->checked=child->attributes.count(L"checked")!=0;else{child->SetAttribute(L"value",L"");child->files.clear();child->selectionStart=child->selectionEnd=0;child->selectionDirection=L"none";}}reset(child);}};reset(node);r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();
            });
            if((key==L"showModal"||key==L"show")&&node->tag==L"dialog")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>&){node->SetAttribute(L"open",L"");node->modal=key==L"showModal";r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();});
            if(key==L"close"&&node->tag==L"dialog")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){node->RemoveAttribute(L"open");node->modal=false;node->SetAttribute(L"returnvalue",a.empty()?L"":r.String(a[0]));r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);r.Dispatch(node,L"close");return Value::Undefined();});
            if(key==L"requestSubmit"&&node->tag==L"form")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){r.Dispatch(node,L"submit");return Value::Undefined();});
            if(key==L"scrollTo"||key==L"scroll"||key==L"scrollBy")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const bool relative=key==L"scrollBy";double left=relative?0:node->scrollLeft,top=relative?0:node->scrollTop;
                if(!a.empty()){
                    auto value=r.Deref(a[0]);
                    if(value.type==Value::Type::Object&&value.object){
                        if(value.object->props.count(L"left"))left=r.Number(value.object->props[L"left"]);
                        if(value.object->props.count(L"top"))top=r.Number(value.object->props[L"top"]);
                    }else{
                        left=r.Number(value);top=a.size()>1?r.Number(a[1]):0;
                    }
                }
                node->scrollLeft=std::max(0.0f,static_cast<float>((relative?node->scrollLeft:0)+left));node->scrollTop=std::max(0.0f,static_cast<float>((relative?node->scrollTop:0)+top));r.Mutated(node,JavaScriptRuntime::MutationKind::Paint);return Value::Undefined();
            });
            if(key==L"scrollIntoView")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(!r.geometryProvider)return Value::Undefined();const auto target=r.geometryProvider(node);
                for(auto ancestor=node->parent.lock();ancestor;ancestor=ancestor->parent.lock()){
                    const auto geometry=r.geometryProvider(ancestor);if(geometry.scrollHeight>geometry.clientHeight+0.01){
                        if(target.y<geometry.y)ancestor->scrollTop=std::max(0.0f,ancestor->scrollTop+static_cast<float>(target.y-geometry.y));
                        else if(target.y+target.height>geometry.y+geometry.clientHeight)ancestor->scrollTop=std::max(0.0f,ancestor->scrollTop+static_cast<float>(target.y+target.height-geometry.y-geometry.clientHeight));
                        r.Mutated(ancestor,JavaScriptRuntime::MutationKind::Paint);
                    }
                }return Value::Undefined();
            });
            if(key==L"setPointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty()||static_cast<int>(r.Number(a[0]))==1){r.pointerCaptureNode=node;if(r.pointerCaptureSink)r.pointerCaptureSink(true);}return Value::Undefined();});
            if(key==L"hasPointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool((a.empty()||static_cast<int>(r.Number(a[0]))==1)&&r.pointerCaptureNode.lock()==node);});
            if(key==L"releasePointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if((a.empty()||static_cast<int>(r.Number(a[0]))==1)&&r.pointerCaptureNode.lock()==node){r.pointerCaptureNode.reset();if(r.pointerCaptureSink)r.pointerCaptureSink(false);}return Value::Undefined();});
            if(key==L"createTBody"&&node->tag==L"table")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){auto body=r.document.CreateElement(L"tbody");body->parent=node;node->children.push_back(body);const bool indexOk=r.document.IndexSubtree(body);r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(body);});
            if(key==L"insertRow"&&(node->tag==L"table"||node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot"))return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto container=node;if(node->tag==L"table"){for(const auto& child:node->children)if(child->tag==L"tbody"){container=child;break;}if(container==node){container=r.document.CreateElement(L"tbody");container->parent=node;node->children.push_back(container);}}
                std::vector<std::shared_ptr<Node>> rows;for(const auto& child:container->children)if(child->tag==L"tr")rows.push_back(child);
                long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?rows.size():std::min(rows.size(),static_cast<size_t>(requested));
                auto row=r.document.CreateElement(L"tr");row->parent=container;auto position=container->children.end();if(index<rows.size())position=std::find(container->children.begin(),container->children.end(),rows[index]);container->children.insert(position,row);
                const bool indexOk=r.document.IndexSubtree(row);r.Mutated(container,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(row);
            });
            if(key==L"deleteRow"&&(node->tag==L"table"||node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot"))return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<std::shared_ptr<Node>> rows;std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& current){for(const auto& child:current->children){if(child->tag==L"tr")rows.push_back(child);else if(current->tag==L"table"&&(child->tag==L"thead"||child->tag==L"tbody"||child->tag==L"tfoot"))collect(child);}};collect(node);if(rows.empty())return Value::Undefined();
                long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?rows.size()-1:static_cast<size_t>(requested);if(index>=rows.size())return Value::Undefined();auto row=rows[index];auto parent=row->parent.lock();const bool indexOk=r.document.UnindexSubtree(row);parent->children.erase(std::remove(parent->children.begin(),parent->children.end(),row),parent->children.end());row->parent.reset();r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"insertCell"&&node->tag==L"tr")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<std::shared_ptr<Node>> cells;for(const auto& child:node->children)if(child->tag==L"td"||child->tag==L"th")cells.push_back(child);long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?cells.size():std::min(cells.size(),static_cast<size_t>(requested));auto cell=r.document.CreateElement(L"td");cell->parent=node;auto position=node->children.end();if(index<cells.size())position=std::find(node->children.begin(),node->children.end(),cells[index]);node->children.insert(position,cell);const bool indexOk=r.document.IndexSubtree(cell);r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(cell);});
            if(key==L"deleteCell"&&node->tag==L"tr")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<std::shared_ptr<Node>> cells;for(const auto& child:node->children)if(child->tag==L"td"||child->tag==L"th")cells.push_back(child);if(cells.empty())return Value::Undefined();long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?cells.size()-1:static_cast<size_t>(requested);if(index>=cells.size())return Value::Undefined();auto cell=cells[index];const bool indexOk=r.document.UnindexSubtree(cell);node->children.erase(std::remove(node->children.begin(),node->children.end(),cell),node->children.end());cell->parent.reset();r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();});
            if(key==L"contains")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Bool(false);auto value=r.Deref(a[0]);if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Bool(false);for(auto current=value.object->node;current;current=current->parent.lock())if(current==node)return Value::Bool(true);return Value::Bool(false);});
            if(key==L"addEventListener")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto& listeners=r.listeners[node.get()];listeners.node=node;r.AddEventListener(listeners.events,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.listeners.find(node.get());if(found!=r.listeners.end()){r.RemoveEventListener(found->second.events,a);if(found->second.events.empty())r.listeners.erase(node.get());}return Value::Undefined();});
            if(key==L"querySelector"||key==L"querySelectorAll")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();auto selector=a.empty()?L"":r.String(a[0]);if(key==L"querySelector")return r.NodeValue(r.document.QuerySelector(selector,node));std::vector<Value> out;for(auto& n:r.document.QuerySelectorAll(selector,node))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"closest")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(node->Closest(a.empty()?L"":r.String(a[0])));});
            if(key==L"matches")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&Document::MatchesSelector(node,r.String(a[0])));});
        }
        if(object->kind==ObjectKind::ClassList){
            auto node=object->node;
            if(key==L"length"){size_t count=0;std::wistringstream classes(node?node->Attribute(L"class"):L"");std::wstring token;while(classes>>token)++count;return Value::Number(static_cast<double>(count));}
            if(key==L"add"||key==L"remove")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                for(auto& v:a)if(key==L"add")node->AddClass(r.String(v));else node->RemoveClass(r.String(v));
                r.Mutated(node,JavaScriptRuntime::MutationKind::Style);return Value::Undefined();
            });
            if(key==L"toggle")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto name=r.String(a[0]);
                bool has=a.size()>1;
                bool force=has?r.Truth(a[1]):false;node->ToggleClass(name,force,has);
                r.Mutated(node,JavaScriptRuntime::MutationKind::Style);return Value::Bool(node->HasClass(name));
            });
            if(key==L"contains")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&node->HasClass(r.String(a[0])));});
        }
        if(object->kind==ObjectKind::Style){
            if(key==L"length")return Value::Number(object->node?static_cast<double>(object->node->inlineStyle.size()):0);
            if(key==L"setProperty")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(object->node&&a.size()>1){object->node->inlineStyle[ToLower(r.String(a[0]))]=r.String(a[1]);r.Mutated(object->node,JavaScriptRuntime::MutationKind::Style);}
                return Value::Undefined();
            });
            if(key==L"getPropertyValue")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->node||a.empty())return Value::String(L"");
                const auto name=ToLower(r.String(a[0]));if(object->props.count(L"$computed")&&r.stylePropertyProvider)return Value::String(r.stylePropertyProvider(object->node,name));
                const auto found=object->node->inlineStyle.find(name);return Value::String(found==object->node->inlineStyle.end()?L"":found->second);
            });
            if(!object->node)return Value::String(L"");const auto name=CamelToKebab(key);if(object->props.count(L"$computed")&&stylePropertyProvider)return Value::String(stylePropertyProvider(object->node,name));const auto found=object->node->inlineStyle.find(name);return Value::String(found==object->node->inlineStyle.end()?L"":found->second);
        }
        if(object->kind==ObjectKind::Dataset)
            return Value::String(object->node?object->node->Attribute(DatasetAttributeName(key)):L"");
        if(object->kind==ObjectKind::Window){
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.windowListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.windowListeners,a);return Value::Undefined();});
            if(key==L"dispatchEvent")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto eventValue=r.Deref(a[0]);
                if(eventValue.type!=Value::Type::Object||!eventValue.object||eventValue.object->kind!=ObjectKind::Event)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"dispatchEvent requires an Event")};
                return Value::Bool(r.DispatchWindowEventObject(eventValue.object));
            });
        }
        if(object->kind==ObjectKind::FrameWindow&&key==L"postMessage")return Native([node=object->node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(!a.empty()){
                const auto data=r.Json(a[0]);
                if(node){if(r.frameMessageSink)r.frameMessageSink(node,data);}
                else if(r.parentMessageSink)r.parentMessageSink(data);
            }
            return Value::Undefined();
        });
        if(object->kind==ObjectKind::Event){
            if(key==L"stopPropagation")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props[L"$propagationStopped"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"stopImmediatePropagation")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props[L"$propagationStopped"]=Value::Bool(true);object->props[L"$immediateStopped"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"preventDefault")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto passive=object->props.find(L"$inPassiveListener");if(passive==object->props.end()||!r.Truth(passive->second))object->props[L"defaultPrevented"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"composedPath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> path;auto target=object->props.find(L"target");if(target!=object->props.end()){auto value=r.Deref(target->second);if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)for(auto node=value.object->node;node;node=node->parent.lock())path.push_back(r.NodeValue(node));}return r.ArrayValue(path);});
        }
        if(object->kind==ObjectKind::WebView){
            if(key==L"postMessage")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(r.messageSink&&!a.empty()){
                    const auto value=r.Deref(a[0]);
                    r.messageSink(value.type==Value::Type::Object?r.Json(value):r.String(value));
                }
                return Value::Undefined();
            });
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.webViewListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.webViewListeners,a);return Value::Undefined();});
        }
        if(object->kind==ObjectKind::Storage){
            if(key==L"getItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Null();const auto found=object->props.find(r.String(a[0]));return found==object->props.end()?Value::Null():found->second;});
            if(key==L"setItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.size()>1)object->props[r.String(a[0])]=Value::String(r.String(a[1]));return Value::Undefined();});
            if(key==L"removeItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())object->props.erase(r.String(a[0]));return Value::Undefined();});
        }
        if(object->kind==ObjectKind::FileReader){
            if(key==L"addEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.objectListeners[object.get()],a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end())r.RemoveEventListener(found->second,a);return Value::Undefined();});
            if(key==L"readAsDataURL")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto dispatch=[&](const std::wstring& type){auto event=r.CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(type);event->props[L"target"]=Value::FromObject(object);event->props[L"currentTarget"]=Value::FromObject(object);event->props[L"defaultPrevented"]=Value::Bool(false);const auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end())r.InvokeEventListeners(found->second,type,false,Value::FromObject(object),Value::FromObject(event),event);};
                if(a.empty()){object->props[L"error"]=r.ErrorValue(L"TypeError",L"FileReader requires a File");dispatch(L"error");return Value::Undefined();}
                const auto file=r.Deref(a[0]);if(file.type!=Value::Type::Object||!file.object||file.object->kind!=ObjectKind::File){object->props[L"error"]=r.ErrorValue(L"TypeError",L"FileReader requires a File");dispatch(L"error");return Value::Undefined();}
                const auto path=r.String(file.object->props[L"$path"]);std::ifstream input(path,std::ios::binary);
                if(!input){object->props[L"error"]=r.ErrorValue(L"NotFoundError",L"File could not be read");dispatch(L"error");return Value::Undefined();}
                std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string encoded;encoded.reserve((bytes.size()+2)/3*4);
                for(size_t index=0;index<bytes.size();index+=3){const unsigned first=static_cast<unsigned char>(bytes[index]);const unsigned second=index+1<bytes.size()?static_cast<unsigned char>(bytes[index+1]):0;const unsigned third=index+2<bytes.size()?static_cast<unsigned char>(bytes[index+2]):0;const unsigned value=(first<<16)|(second<<8)|third;encoded.push_back(alphabet[(value>>18)&63]);encoded.push_back(alphabet[(value>>12)&63]);encoded.push_back(index+1<bytes.size()?alphabet[(value>>6)&63]:'=');encoded.push_back(index+2<bytes.size()?alphabet[value&63]:'=');}
                const auto type=r.String(file.object->props[L"type"]);object->props[L"result"]=Value::String(L"data:"+(type.empty()?std::wstring(L"application/octet-stream"):type)+L";base64,"+Utf8ToWide(encoded));object->props[L"error"]=Value::Null();dispatch(L"load");return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Clipboard){
            if(key==L"writeText")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto promise=r.PromiseValue();const auto text=a.empty()?L"":r.String(a[0]);bool success=false;if(OpenClipboard(nullptr)){if(EmptyClipboard()){const SIZE_T bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(memory){if(void* target=GlobalLock(memory)){std::memcpy(target,text.c_str(),bytes);GlobalUnlock(memory);if(SetClipboardData(CF_UNICODETEXT,memory))success=true;else GlobalFree(memory);}else GlobalFree(memory);}}CloseClipboard();}if(success)r.ResolvePromise(promise.object,Value::Undefined());else r.RejectPromise(promise.object,r.ErrorValue(L"NotAllowedError",L"Clipboard is unavailable"));return promise;});
            if(key==L"readText")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){auto promise=r.PromiseValue();std::wstring text;bool success=false;if(OpenClipboard(nullptr)){if(HANDLE data=GetClipboardData(CF_UNICODETEXT)){if(const auto* value=static_cast<const wchar_t*>(GlobalLock(data))){text=value;GlobalUnlock(data);success=true;}}CloseClipboard();}if(success)r.ResolvePromise(promise.object,Value::String(text));else r.RejectPromise(promise.object,r.ErrorValue(L"NotAllowedError",L"Clipboard is unavailable"));return promise;});
        }
        if(object->kind==ObjectKind::DataTransfer&&key==L"getData")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto type=a.empty()?L"":ToLower(r.String(a[0]));if(type==L"text"||type==L"text/plain"){const auto found=object->props.find(L"$text");return Value::String(found==object->props.end()?L"":r.String(found->second));}return Value::String(L"");});
        if(object->kind==ObjectKind::DataTransferItem&&key==L"getAsFile")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$file");return found==object->props.end()?Value::Null():found->second;});
        if(object->kind==ObjectKind::UrlSearchParams&&key==L"get")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Null();const auto found=object->props.find(r.String(a[0]));return found==object->props.end()?Value::Null():found->second;});
        if(object->kind==ObjectKind::Response&&key==L"text")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$body");return r.PromiseResolveValue(found==object->props.end()?Value::String(L""):Value::String(r.String(found->second)));});
        if(object->kind==ObjectKind::Response&&key==L"json")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$body");if(found==object->props.end())return r.PromiseResolveValue(r.ObjectValue(ObjectKind::Plain));try{Compiler parser(r.module,r.String(found->second));return r.PromiseResolveValue(r.Run(parser.CompileExpressionOnly(),r.global));}catch(const JavaScriptException& exception){auto promise=r.PromiseValue();r.RejectPromise(promise.object,exception.value);return promise;}catch(const std::exception& exception){auto promise=r.PromiseValue();r.RejectPromise(promise.object,r.ErrorValue(L"SyntaxError",Utf8ToWide(exception.what())));return promise;}});
        if(object->kind==ObjectKind::Performance&&key==L"now")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){using namespace std::chrono;return Value::Number(duration<double,std::milli>(steady_clock::now().time_since_epoch()).count());});
        if(object->kind==ObjectKind::Math){
            if(key==L"PI")return Value::Number(3.14159265358979323846);
            if(key==L"E")return Value::Number(2.71828182845904523536);
            if(key==L"floor"||key==L"ceil"||key==L"round"||key==L"log"||key==L"pow"||
               key==L"min"||key==L"max"||key==L"abs"||key==L"sqrt"||key==L"sin"||
               key==L"cos"||key==L"tan"||key==L"atan2")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const double first=a.empty()?0:r.Number(a[0]);
                if(key==L"floor")return Value::Number(std::floor(first));if(key==L"ceil")return Value::Number(std::ceil(first));
                if(key==L"round")return Value::Number(std::round(first));if(key==L"log")return Value::Number(std::log(first));
                if(key==L"abs")return Value::Number(std::abs(first));if(key==L"sqrt")return Value::Number(std::sqrt(first));
                if(key==L"sin")return Value::Number(std::sin(first));if(key==L"cos")return Value::Number(std::cos(first));
                if(key==L"tan")return Value::Number(std::tan(first));
                if(key==L"atan2")return Value::Number(std::atan2(first,a.size()>1?r.Number(a[1]):0));
                if(key==L"pow")return Value::Number(std::pow(first,a.size()>1?r.Number(a[1]):0));
                double number=first;for(size_t i=1;i<a.size();++i)number=key==L"min"?std::min(number,r.Number(a[i])):std::max(number,r.Number(a[i]));return Value::Number(number);
            });
        }
        if(object->kind==ObjectKind::Json&&(key==L"stringify"||key==L"parse"))return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(key==L"stringify")return Value::String(a.empty()?L"undefined":r.Json(a[0]));if(a.empty())return Value::Undefined();try{Compiler parser(r.module,r.String(a[0]));return r.Run(parser.CompileExpressionOnly(),r.global);}catch(...){return Value::Undefined();}});
        if(object->kind==ObjectKind::ArrayConstructor&&(key==L"from"||key==L"isArray"))return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(key==L"isArray")return Value::Bool(!a.empty()&&r.Deref(a[0]).type==Value::Type::Object&&r.Deref(a[0]).object->kind==ObjectKind::Array);if(a.empty())return r.ArrayValue({});auto v=r.Deref(a[0]);std::vector<Value> values;if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::Array)values=v.object->items;else if(v.type==Value::Type::Object&&v.object){const auto length=v.object->props.find(L"length");const auto count=length==v.object->props.end()?0:static_cast<size_t>(std::max(0.0,r.Number(length->second)));values.resize(count,Value::Undefined());}if(a.size()>1)for(size_t i=0;i<values.size();++i)values[i]=r.Deref(r.Call(a[1],Value::Undefined(),{values[i],Value::Number(static_cast<double>(i))}));return r.ArrayValue(values);});
        if(object->kind==ObjectKind::NumberConstructor&&
           (key==L"isFinite"||key==L"isNaN"||key==L"isInteger"))
            return Native([key](RuntimeCore& r,const Value&,
                                const std::vector<Value>& arguments){
                const auto value=arguments.empty()?Value::Undefined():r.Deref(arguments[0]);
                if(value.type!=Value::Type::Number)return Value::Bool(false);
                if(key==L"isFinite")return Value::Bool(std::isfinite(value.number));
                if(key==L"isNaN")return Value::Bool(std::isnan(value.number));
                return Value::Bool(std::isfinite(value.number)&&
                                   std::trunc(value.number)==value.number);
            });
        return Value::Undefined();
    }
    struct ParsedEventListenerOptions { bool capture=false,once=false,passive=false; };
    ParsedEventListenerOptions EventListenerOptions(const Value& input){
        ParsedEventListenerOptions options;const auto value=Deref(input);
        if(value.type==Value::Type::Boolean){options.capture=value.boolean;return options;}
        if(value.type!=Value::Type::Object||!value.object)return options;
        options.capture=Truth(GetProperty(value,L"capture"));
        options.once=Truth(GetProperty(value,L"once"));
        options.passive=Truth(GetProperty(value,L"passive"));
        return options;
    }
    bool IsEventCallback(const Value& input){
        const auto callback=Deref(input);if(IsCallable(callback))return true;
        return callback.type==Value::Type::Object&&callback.object&&
            IsCallable(GetProperty(callback,L"handleEvent"));
    }
    void AddEventListener(EventListenerMap& map,const std::vector<Value>& arguments){
        if(arguments.size()<2||!IsEventCallback(arguments[1]))return;
        const auto type=String(arguments[0]);const auto callback=Deref(arguments[1]);
        const auto options=arguments.size()>2?EventListenerOptions(arguments[2]):ParsedEventListenerOptions{};
        auto& entries=map[type];
        for(const auto& entry:entries)
            if(entry.capture==options.capture&&EqualValues(entry.callback,callback))return;
        entries.push_back({nextEventListenerId++,callback,options.capture,options.once,options.passive});
    }
    void RemoveEventListener(EventListenerMap& map,const std::vector<Value>& arguments){
        if(arguments.size()<2)return;const auto found=map.find(String(arguments[0]));if(found==map.end())return;
        const auto callback=Deref(arguments[1]);const bool capture=arguments.size()>2?EventListenerOptions(arguments[2]).capture:false;
        auto& entries=found->second;
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const EventListener& entry){return entry.capture==capture&&EqualValues(entry.callback,callback);}),entries.end());
        if(entries.empty())map.erase(String(arguments[0]));
    }
    void CallEventCallback(const Value& callback,const Value& currentTarget,const Value& eventValue){
        const auto value=Deref(callback);
        if(IsCallable(value)){Call(value,currentTarget,{eventValue});return;}
        if(value.type==Value::Type::Object&&value.object){const auto handler=GetProperty(value,L"handleEvent");if(IsCallable(handler))Call(handler,value,{eventValue});}
    }
    void InvokeEventListeners(EventListenerMap& map,const std::wstring& type,bool capture,
                              const Value& currentTarget,const Value& eventValue,
                              const std::shared_ptr<Object>& event){
        const auto immediate=event->props.find(L"$immediateStopped");
        if(immediate!=event->props.end()&&Truth(immediate->second))return;
        const auto found=map.find(type);if(found==map.end())return;
        const auto snapshot=found->second;
        for(const auto& entry:snapshot){
            auto live=map.find(type);if(live==map.end())break;
            const auto position=std::find_if(live->second.begin(),live->second.end(),[&](const EventListener& candidate){return candidate.id==entry.id;});
            if(position==live->second.end()||position->capture!=capture)continue;
            const auto callback=position->callback;const bool passive=position->passive;
            if(position->once){live->second.erase(position);if(live->second.empty())map.erase(type);}
            event->props[L"$inPassiveListener"]=Value::Bool(passive);
            try{CallEventCallback(callback,currentTarget,eventValue);}
            catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
            catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
            event->props[L"$inPassiveListener"]=Value::Bool(false);
            const auto stopped=event->props.find(L"$immediateStopped");if(stopped!=event->props.end()&&Truth(stopped->second))break;
        }
    }
    void SetProperty(const Value& input,const std::wstring& key,const Value& value){
        auto base=Deref(input);if(base.type!=Value::Type::Object||!base.object)return;auto object=base.object;auto v=Deref(value);
        if(object->kind==ObjectKind::Array){
            if(key==L"length"){
                const double requested=Number(v);
                if(!std::isfinite(requested)||requested<0||std::floor(requested)!=requested||
                   requested>static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
                    throw JavaScriptException{ErrorValue(L"RangeError",L"Invalid array length")};
                object->items.resize(static_cast<size_t>(requested));
                object->props.erase(L"length");return;
            }
            size_t i=0;if(TryParseDecimalIndex(key,i)){if(i>=object->items.size())object->items.resize(i+1);object->items[i]=v;return;}
        }
        if(object->kind==ObjectKind::Location&&key==L"hash"){
            auto hash=String(v);if(!hash.empty()&&hash.front()!=L'#')hash.insert(hash.begin(),L'#');
            const auto found=object->props.find(L"hash");const auto previous=found==object->props.end()?L"":String(found->second);
            if(hash==previous)return;
            auto baseLocation=location;const auto fragment=baseLocation.find(L'#');if(fragment!=std::wstring::npos)baseLocation.resize(fragment);
            location=baseLocation+hash;object->props[L"hash"]=Value::String(hash);object->props[L"href"]=Value::String(location);
            DispatchWindow(L"hashchange");return;
        }
        if(object->kind==ObjectKind::Node&&object->node){auto n=object->node;bool indexOk=true;
            if(n->tag==L"canvas"&&(key==L"width"||key==L"height")){
                const double number=Number(v);n->SetAttribute(key,std::isfinite(number)&&number>=0?NumberString(std::floor(number)):L"0");
                ResetCanvas(n);Mutated(n,JavaScriptRuntime::MutationKind::Layout);return;
            }
            if(n->tag==L"img"&&(key==L"width"||key==L"height")){
                const double number=Number(v);
                n->SetAttribute(key,std::isfinite(number)&&number>=0?
                    NumberString(std::floor(number)):L"0");
                Mutated(n,JavaScriptRuntime::MutationKind::Layout);return;
            }
            if(key==L"id"){const auto oldId=n->Attribute(L"id");n->SetAttribute(L"id",String(v));indexOk=document.UpdateElementId(n,oldId,n->Attribute(L"id"));}else if(key==L"className")n->SetAttribute(L"class",String(v));else if(key==L"value"){
                const auto text=String(v);if(n->tag==L"input"&&ToLower(n->Attribute(L"type"))==L"file"&&text.empty())n->files.clear();
                n->SetAttribute(L"value",text);n->selectionStart=std::min(n->selectionStart,text.size());n->selectionEnd=std::min(n->selectionEnd,text.size());
                if(n->selectionStart==n->selectionEnd)n->selectionDirection=L"none";
            }
            else if(key==L"selectionStart"||key==L"selectionEnd"){
                const auto length=n->Attribute(L"value").size();const auto position=std::min(length,static_cast<size_t>(std::max(0.0,Number(v))));
                if(key==L"selectionStart")n->selectionStart=position;else n->selectionEnd=position;
                if(selectionSetter)selectionSetter(n,n->selectionStart,n->selectionEnd);return;
            }
            else if(key==L"selectionDirection"){const auto direction=ToLower(String(v));n->selectionDirection=direction==L"forward"||direction==L"backward"?direction:L"none";return;}
            else if(key==L"title"||key==L"type"||key==L"lang"||key==L"src"||key==L"href"||
                    key==L"name"||key==L"placeholder"||key==L"rel"||key==L"alt"||
                    key==L"colSpan"||key==L"rowSpan"||key==L"returnValue")
                {n->SetAttribute(key==L"colSpan"?L"colspan":(key==L"rowSpan"?L"rowspan":ToLower(key)),String(v));
                 if(key==L"src")ResetImageSourceState(n);}
            else if(key==L"draggable")n->SetAttribute(L"draggable",Truth(v)?L"true":L"false");
            else if(key==L"contentEditable")n->SetAttribute(L"contenteditable",String(v));
            else if(key==L"hidden"||key==L"open"||key==L"defer"||key==L"required"){if(Truth(v))n->SetAttribute(ToLower(key),L"");else n->RemoveAttribute(ToLower(key));if(n->tag==L"dialog"&&key==L"open")n->modal=false;Mutated(n,JavaScriptRuntime::MutationKind::Style);return;}
            else if(key==L"scrollTop"){n->scrollTop=std::max(0.0f,static_cast<float>(Number(v)));Mutated(n,JavaScriptRuntime::MutationKind::Paint);return;}
            else if(key==L"scrollLeft"){n->scrollLeft=std::max(0.0f,static_cast<float>(Number(v)));Mutated(n,JavaScriptRuntime::MutationKind::Paint);return;}
            else if(key==L"checked")n->checked=Truth(v);
            else if(key==L"indeterminate")n->indeterminate=Truth(v);
            else if(key==L"disabled")n->disabled=Truth(v);
            else if(key==L"innerText"||key==L"textContent"){for(const auto& child:n->children)indexOk=document.UnindexSubtree(child)&&indexOk;n->SetInnerText(String(v));Mutated(n,JavaScriptRuntime::MutationKind::Tree,!indexOk);return;}else if(key==L"innerHTML"){for(const auto& child:n->children)indexOk=document.UnindexSubtree(child)&&indexOk;document.SetInnerHtml(n,String(v),false);for(const auto& child:n->children)indexOk=document.IndexSubtree(child)&&indexOk;Mutated(n,JavaScriptRuntime::MutationKind::Tree,!indexOk);return;}else object->props[key]=v;
            const bool requiresLayout=key==L"value"||key==L"type"||key==L"lang"||key==L"placeholder"||key==L"src"||
                key==L"colSpan"||key==L"rowSpan";
            Mutated(n,requiresLayout?JavaScriptRuntime::MutationKind::Layout:
                JavaScriptRuntime::MutationKind::Style,!indexOk);return;}
        if(object->kind==ObjectKind::Style&&object->node){object->node->inlineStyle[CamelToKebab(key)]=String(v);Mutated(object->node,JavaScriptRuntime::MutationKind::Style);return;}
        if(object->kind==ObjectKind::Dataset&&object->node){object->node->SetAttribute(DatasetAttributeName(key),String(v));Mutated(object->node,JavaScriptRuntime::MutationKind::Style);return;}
        if(object->kind==ObjectKind::CanvasContext2D&&object->node){
            const auto surface=EnsureCanvas(object->node);if(!surface)return;auto& state=surface->state;
            if(key==L"fillStyle"||key==L"strokeStyle"){
                auto& paint=key==L"fillStyle"?state.fillStyle:state.strokeStyle;
                if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::CanvasGradient&&v.object->canvasGradient){
                    paint.gradient=v.object->canvasGradient;paint.color.clear();
                }else{paint.color=String(v);paint.gradient.reset();}
            }else if(key==L"lineWidth"){
                const double width=Number(v);if(std::isfinite(width)&&width>0)state.lineWidth=static_cast<float>(width);
            }else if(key==L"font")state.font=String(v);
            else if(key==L"textAlign")state.textAlign=ToLower(Trim(String(v)));
            else if(key==L"textBaseline")state.textBaseline=ToLower(Trim(String(v)));
            else object->props[key]=v;
            return;
        }
        object->props[key]=v;
    }
    void Assign(const Value& reference,const Value& value){
        if(reference.type!=Value::Type::Reference)return;auto ref=reference.reference;
        if(ref->kind==Reference::Kind::Variable){auto* env=ref->env->Find(ref->name);if(!env)env=ref->env.get();env->values[ref->name]=Deref(value);}else SetProperty(ref->base,ref->name,value);
    }
    bool Delete(const Value& reference){
        if(reference.type!=Value::Type::Reference)return true;const auto ref=reference.reference;
        if(ref->kind==Reference::Kind::Variable){if(ref->env){auto* env=ref->env->Find(ref->name);if(env)env->values.erase(ref->name);}return true;}
        auto base=Deref(ref->base);if(base.type!=Value::Type::Object||!base.object)return true;
        if(base.object->kind==ObjectKind::Dataset&&base.object->node){base.object->node->RemoveAttribute(DatasetAttributeName(ref->name));Mutated(base.object->node,JavaScriptRuntime::MutationKind::Style);return true;}
        base.object->props.erase(ref->name);return true;
    }
    Value Call(const Value& callable,const Value& explicitThis,const std::vector<Value>& args){
        Value callee=Deref(callable),thisValue=explicitThis;
        if(callable.type==Value::Type::Reference&&callable.reference->kind==Reference::Kind::Property)thisValue=Deref(callable.reference->base);
        if(callee.type==Value::Type::Native)return callee.native->callback(*this,thisValue,args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::NumberConstructor)
            return Value::Number(args.empty()?0:Number(args[0]));
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ArrayConstructor){
            if(args.size()==1){const auto value=Deref(args[0]);if(value.type==Value::Type::Number&&std::isfinite(value.number)&&value.number>=0&&std::floor(value.number)==value.number){std::vector<Value> items(static_cast<size_t>(value.number),Value::Undefined());return ArrayValue(items);}}
            return ArrayValue(args);
        }
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::DateConstructor)return DateValue(args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ErrorConstructor){const auto name=callee.object->props.count(L"$name")?String(callee.object->props[L"$name"]):L"Error";return ErrorValue(name,args.empty()?L"":String(args[0]));}
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::PromiseConstructor)throw JavaScriptException{ErrorValue(L"TypeError",L"Promise constructor must be called with new")};
        if(callee.type==Value::Type::Function){
            const auto prototype=callee.function->prototype;auto env=CreateEnvironment();env->parent=callee.function->closure;
            env->values.reserve(prototype->parameters.size()+3);
            try{
                if(!prototype->lexicalThis)env->values[L"this"]=thisValue;
                if(!prototype->name.empty())env->values[prototype->name]=callee;
                for(size_t i=0;i<prototype->parameters.size();++i){auto value=i<args.size()?Deref(args[i]):Value::Undefined();env->values[prototype->parameters[i]]=value;if(value.type==Value::Type::Undefined&&i<prototype->parameterDefaults.size()&&prototype->parameterDefaults[i])env->values[prototype->parameters[i]]=Run(*prototype->parameterDefaults[i],env);}
                if(!prototype->lexicalThis)env->values[L"arguments"]=ArrayValue(args);
                if(!prototype->restParameter.empty()){
                    std::vector<Value> rest;
                    if(args.size()>prototype->parameters.size())rest.assign(args.begin()+prototype->parameters.size(),args.end());
                    env->values[prototype->restParameter]=ArrayValue(rest);
                }
                for(const auto& binding:prototype->bindings){
                    auto value=GetProperty(env->values[binding.parameter],binding.property);
                    if(Deref(value).type==Value::Type::Undefined&&binding.defaultValue)
                        value=Run(*binding.defaultValue,env);
                    env->values[binding.name]=Deref(value);
                }
                if(prototype->isAsync)return StartAsyncFunction(prototype,env);return Run(prototype->chunk,env);
            }catch(const JavaScriptException& exception){if(!prototype->isAsync)throw;auto promise=PromiseValue();RejectPromise(promise.object,exception.value);return promise;}
            catch(const std::exception& exception){if(!prototype->isAsync)throw;auto promise=PromiseValue();RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));return promise;}
        }
        return Value::Undefined();
    }
    Value Construct(const Value& constructor,const std::vector<Value>& args){
        const auto callee=Deref(constructor);
        if(callee.type==Value::Type::Native){const auto result=Call(callee,Value::Undefined(),args);return Deref(result);}
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::DateConstructor)return DateValue(args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::PromiseConstructor)return ConstructPromise(args.empty()?Value::Undefined():args[0]);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ErrorConstructor){const auto name=callee.object->props.count(L"$name")?String(callee.object->props[L"$name"]):L"Error";return ErrorValue(name,args.empty()?L"":String(args[0]));}
        auto instance=ObjectValue(ObjectKind::Plain);
        if(callee.type==Value::Type::Object&&callee.object){
            instance.object->prototype=callee.object;
            const auto found=callee.object->props.find(L"$method:constructor");
            if(found!=callee.object->props.end())Call(found->second,instance,args);
            return instance;
        }
        if(callee.type==Value::Type::Function){
            const auto result=Deref(Call(callee,instance,args));
            return result.type==Value::Type::Object?result:instance;
        }
        return instance;
    }
    bool EqualValues(const Value& a,const Value& b){auto x=Deref(a),y=Deref(b);if(x.type!=y.type)return false;switch(x.type){case Value::Type::Undefined:case Value::Type::Null:return true;case Value::Type::Boolean:return x.boolean==y.boolean;case Value::Type::Number:return x.number==y.number;case Value::Type::String:return x.string==y.string;case Value::Type::Object:return x.object==y.object;case Value::Type::Function:return x.function==y.function;case Value::Type::Native:return x.native==y.native;default:return false;}}
    bool LooseEqualValues(const Value& a,const Value& b){auto x=Deref(a),y=Deref(b);if((x.type==Value::Type::Undefined||x.type==Value::Type::Null)&&(y.type==Value::Type::Undefined||y.type==Value::Type::Null))return true;if(x.type==y.type)return EqualValues(x,y);if(x.type==Value::Type::Boolean||x.type==Value::Type::Number||x.type==Value::Type::String||y.type==Value::Type::Boolean||y.type==Value::Type::Number||y.type==Value::Type::String)return Number(x)==Number(y);return false;}
    std::wstring Json(const Value& input){
        auto v=Deref(input);if(v.type==Value::Type::Undefined)return L"undefined";if(v.type==Value::Type::Null)return L"null";if(v.type==Value::Type::Boolean)return v.boolean?L"true":L"false";if(v.type==Value::Type::Number)return NumberString(v.number);
        if(v.type==Value::Type::String){std::wstring o=L"\"";for(wchar_t c:v.string){if(c==L'\\'||c==L'"')o+=L'\\';if(c==L'\n')o+=L"\\n";else o+=c;}return o+L"\"";}
        if(v.type==Value::Type::Object&&v.object){if(v.object->kind==ObjectKind::Array){std::wstring o=L"[";for(size_t i=0;i<v.object->items.size();++i){if(i)o+=L",";o+=Json(v.object->items[i]);}return o+L"]";}std::wstring o=L"{";bool first=true;for(auto& p:v.object->props){if(!first)o+=L",";first=false;o+=Json(Value::String(p.first))+L":"+Json(p.second);}return o+L"}";}return L"undefined";
    }
    std::wstring TypeName(const Value& input){auto v=Deref(input);switch(v.type){case Value::Type::Undefined:return L"undefined";case Value::Type::Boolean:return L"boolean";case Value::Type::Number:return L"number";case Value::Type::String:return L"string";case Value::Type::Function:case Value::Type::Native:return L"function";case Value::Type::Object:if(v.object&&(v.object->kind==ObjectKind::PromiseConstructor||v.object->kind==ObjectKind::ErrorConstructor||v.object->kind==ObjectKind::NumberConstructor||v.object->kind==ObjectKind::DateConstructor))return L"function";return L"object";default:return L"object";}}
    Value EvaluateTemplate(const std::wstring& raw,const std::shared_ptr<Environment>& env){
        auto found=templatePrograms.find(raw);
        if(found==templatePrograms.end()){
            auto program=std::make_shared<TemplateProgram>();size_t position=0;
            while(position<raw.size()){
                const auto begin=raw.find(L"${",position);
                if(begin==std::wstring::npos){program->literals.push_back(raw.substr(position));position=raw.size();break;}
                program->literals.push_back(raw.substr(position,begin-position));
                int depth=1;size_t end=begin+2;wchar_t quote=0;
                for(;end<raw.size()&&depth;++end){const auto c=raw[end];if(quote){if(c==quote&&raw[end-1]!=L'\\')quote=0;}else if(c==L'\''||c==L'"'||c==L'`')quote=c;else if(c==L'{')++depth;else if(c==L'}')--depth;}
                if(depth){program->literals.back()+=raw.substr(begin);position=raw.size();break;}
                Compiler compiler(module,raw.substr(begin+2,end-begin-3));
                program->expressions.push_back(compiler.CompileExpressionOnly());position=end;
            }
            if(program->literals.size()==program->expressions.size())program->literals.emplace_back();
            if(templatePrograms.size()>=256)templatePrograms.clear();
            found=templatePrograms.emplace(raw,program).first;
        }
        const auto& program=*found->second;std::wstring out;size_t reserve=0;
        for(const auto& literal:program.literals)reserve+=literal.size();out.reserve(reserve+32*program.expressions.size());
        for(size_t index=0;index<program.expressions.size();++index){
            out+=program.literals[index];out+=String(Run(program.expressions[index],env));
        }
        if(!program.literals.empty())out+=program.literals.back();
        return Value::String(out);
    }
    static bool InRange(size_t position,size_t begin,size_t end){return position>=begin&&position<end;}
    void CancelPendingFinally(const std::shared_ptr<ExecutionFrame>& frame,size_t origin){
        frame->pending.erase(std::remove_if(frame->pending.begin(),frame->pending.end(),[&](const PendingCompletion& completion){const auto& handler=frame->chunk->handlers[completion.handler];return handler.hasFinally&&InRange(origin,handler.finallyStart,handler.finallyEnd);}),frame->pending.end());
    }
    void LeaveCatchScope(const std::shared_ptr<ExecutionFrame>& frame,int handlerIndex){
        const auto found=std::find_if(frame->catchScopes.rbegin(),frame->catchScopes.rend(),[&](const ExecutionFrame::CatchScope& scope){return scope.handler==handlerIndex;});
        if(found==frame->catchScopes.rend())return;frame->env=found->outer;frame->catchScopes.erase(std::next(found).base());
    }
    bool DispatchException(const std::shared_ptr<ExecutionFrame>& frame,const Value& thrown,size_t origin){
        CancelPendingFinally(frame,origin);
        for(int index=static_cast<int>(frame->chunk->handlers.size())-1;index>=0;--index){const auto& handler=frame->chunk->handlers[static_cast<size_t>(index)];
            if(InRange(origin,handler.tryStart,handler.tryEnd)){
                frame->stack.clear();
                if(handler.hasCatch){auto catchEnvironment=CreateEnvironment();catchEnvironment->parent=frame->env;if(!handler.catchName.empty())catchEnvironment->values[handler.catchName]=Deref(thrown);frame->catchScopes.push_back({index,frame->env});frame->env=catchEnvironment;frame->ip=handler.catchStart;return true;}
                if(handler.hasFinally){frame->pending.push_back({index,CompletionKind::Throw,Deref(thrown),0});frame->ip=handler.finallyStart;return true;}
            }
            if(handler.hasCatch&&InRange(origin,handler.catchStart,handler.catchEnd)){frame->stack.clear();LeaveCatchScope(frame,index);if(handler.hasFinally){frame->pending.push_back({index,CompletionKind::Throw,Deref(thrown),0});frame->ip=handler.finallyStart;return true;}}
        }
        return false;
    }
    bool BeginAbrupt(const std::shared_ptr<ExecutionFrame>& frame,CompletionKind kind,const Value& value,size_t target,size_t origin){
        CancelPendingFinally(frame,origin);
        for(int index=static_cast<int>(frame->chunk->handlers.size())-1;index>=0;--index){const auto& handler=frame->chunk->handlers[static_cast<size_t>(index)];
            const bool inTry=InRange(origin,handler.tryStart,handler.tryEnd),inCatch=handler.hasCatch&&InRange(origin,handler.catchStart,handler.catchEnd);if(!inTry&&!inCatch)continue;
            if(kind==CompletionKind::Jump){const bool stays=inTry?InRange(target,handler.tryStart,handler.tryEnd):InRange(target,handler.catchStart,handler.catchEnd);if(stays)continue;if(target==handler.finallyStart)return false;}
            if(inCatch)LeaveCatchScope(frame,index);if(!handler.hasFinally)continue;
            frame->pending.push_back({index,kind,Deref(value),target});frame->ip=handler.finallyStart;return true;
        }
        return false;
    }
    void ResumeAsync(const std::shared_ptr<ExecutionFrame>& frame,bool rejected,const Value& value){
        if(!frame||!frame->asyncPromise||frame->asyncPromise->promiseState!=PromiseState::Pending)return;
        if(rejected){frame->resumeThrow=true;frame->resumeValue=Deref(value);}else frame->stack.push_back(Deref(value));
        try{const auto result=RunFrame(frame);if(!result.suspended)ResolvePromise(frame->asyncPromise,result.value);}
        catch(const JavaScriptException& exception){RejectPromise(frame->asyncPromise,exception.value);}
        catch(const std::exception& exception){RejectPromise(frame->asyncPromise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
    }
    FrameResult RunFrame(const std::shared_ptr<ExecutionFrame>& frame){
        for(const auto& constant:frame->chunk->constants)TrackValue(constant);
        auto& stack=frame->stack;auto pop=[&](){if(stack.empty())return Value::Undefined();auto value=std::move(stack.back());stack.pop_back();return value;};
        if(frame->resumeThrow){frame->resumeThrow=false;const auto thrown=frame->resumeValue;if(!DispatchException(frame,thrown,frame->resumeOrigin))throw JavaScriptException{thrown};}
        while(frame->ip<frame->chunk->code.size()){
            const size_t current=frame->ip++;const auto& ins=frame->chunk->code[current];
            try{switch(ins.op){
            case Op::Constant:stack.push_back(frame->chunk->constants[ins.argument]);break;case Op::Undefined:stack.push_back(Value::Undefined());break;case Op::Null:stack.push_back(Value::Null());break;case Op::TrueValue:stack.push_back(Value::Bool(true));break;case Op::FalseValue:stack.push_back(Value::Bool(false));break;
            case Op::LoadReference:{auto reference=std::make_shared<Reference>();reference->env=frame->env;reference->name=ins.text;stack.push_back(Value::FromReference(reference));break;}
            case Op::Declare:{auto value=Deref(pop());frame->env->values[ins.text]=value;break;}
            case Op::GetProperty:{auto base=pop();auto reference=std::make_shared<Reference>();reference->kind=Reference::Kind::Property;reference->base=base;reference->name=ins.text;stack.push_back(Value::FromReference(reference));break;}
            case Op::GetIndex:{auto key=String(pop());auto base=pop();auto reference=std::make_shared<Reference>();reference->kind=Reference::Kind::Property;reference->base=base;reference->name=key;stack.push_back(Value::FromReference(reference));break;}
            case Op::Assign:{auto value=Deref(pop());auto reference=pop();Assign(reference,value);stack.push_back(value);break;}
            case Op::PostIncrement:case Op::PostDecrement:case Op::PreIncrement:case Op::PreDecrement:{auto reference=pop();const auto old=Deref(reference);const double delta=(ins.op==Op::PostIncrement||ins.op==Op::PreIncrement)?1.0:-1.0;const auto next=Value::Number(Number(old)+delta);Assign(reference,next);stack.push_back(ins.op==Op::PostIncrement||ins.op==Op::PostDecrement?old:next);break;}
            case Op::NewArray:stack.push_back(ArrayValue({}));break;case Op::ArrayPush:{auto value=Deref(pop());auto array=Deref(stack.back());array.object->items.push_back(value);break;}
            case Op::ArraySpread:{auto source=Deref(pop());auto array=Deref(stack.back());if(source.type==Value::Type::Object&&source.object&&array.type==Value::Type::Object&&array.object){if(source.object->kind==ObjectKind::Array)array.object->items.insert(array.object->items.end(),source.object->items.begin(),source.object->items.end());else if(source.object->kind==ObjectKind::Map||source.object->kind==ObjectKind::Set)for(const auto& entry:source.object->entries)array.object->items.push_back(source.object->kind==ObjectKind::Map?entry.second:entry.first);}break;}
            case Op::NewObject:stack.push_back(ObjectValue(ObjectKind::Plain));break;case Op::ObjectSet:{auto value=Deref(pop());auto object=Deref(stack.back());object.object->props[ins.text]=value;break;}
            case Op::ObjectSpread:{auto source=Deref(pop());auto target=Deref(stack.back());if(source.type==Value::Type::Object&&source.object&&target.type==Value::Type::Object&&target.object)for(const auto& property:source.object->props)target.object->props[property.first]=Deref(property.second);break;}
            case Op::EnumerableKeys:{
                const auto source=Deref(pop());std::vector<Value> keys;
                if(source.type==Value::Type::String){for(size_t index=0;index<source.string.size();++index)keys.push_back(Value::String(std::to_wstring(index)));}
                else if(source.type==Value::Type::Object&&source.object){
                    if(source.object->kind==ObjectKind::Array)for(size_t index=0;index<source.object->items.size();++index)keys.push_back(Value::String(std::to_wstring(index)));
                    for(const auto& property:source.object->props)if(property.first.empty()||property.first.front()!=L'$')keys.push_back(Value::String(property.first));
                }
                stack.push_back(ArrayValue(keys));break;
            }
            case Op::MakeFunction:{auto function=CreateFunction();function->prototype=module->prototypes[ins.argument];function->closure=frame->env;stack.push_back(Value::FromFunction(function));break;}
            case Op::Call:{std::vector<Value> args(ins.argument);for(int i=ins.argument-1;i>=0;--i)args[i]=Deref(pop());auto callee=pop();stack.push_back(Call(callee,Value::Undefined(),args));break;}
            case Op::CallArray:{auto arguments=Deref(pop());auto callee=pop();stack.push_back(Call(callee,Value::Undefined(),arguments.type==Value::Type::Object&&arguments.object?arguments.object->items:std::vector<Value>{}));break;}
            case Op::Construct:{std::vector<Value> args(ins.argument);for(int i=ins.argument-1;i>=0;--i)args[i]=Deref(pop());auto constructor=pop();stack.push_back(Construct(constructor,args));break;}
            case Op::ConstructArray:{auto arguments=Deref(pop());auto constructor=pop();stack.push_back(Construct(constructor,arguments.type==Value::Type::Object&&arguments.object?arguments.object->items:std::vector<Value>{}));break;}
            case Op::DeleteValue:stack.push_back(Value::Bool(Delete(pop())));break;
            case Op::ThrowValue:throw JavaScriptException{Deref(pop())};
            case Op::Await:{const auto awaited=PromiseResolveValue(pop());const auto origin=current;frame->resumeOrigin=origin;auto fulfilled=Native([frame](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){runtime.ResumeAsync(frame,false,values.empty()?Value::Undefined():values[0]);return Value::Undefined();});auto rejected=Native([frame](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){runtime.ResumeAsync(frame,true,values.empty()?Value::Undefined():values[0]);return Value::Undefined();});PerformThen(awaited.object,fulfilled,rejected);return {true,Value::Undefined()};}
            case Op::LeaveCatch:LeaveCatchScope(frame,ins.argument);break;
            case Op::EndFinally:{auto found=std::find_if(frame->pending.rbegin(),frame->pending.rend(),[&](const PendingCompletion& pending){return pending.handler==ins.argument;});if(found==frame->pending.rend())break;auto completion=*found;frame->pending.erase(std::next(found).base());if(completion.kind==CompletionKind::Throw){if(!DispatchException(frame,completion.value,current))throw JavaScriptException{completion.value};break;}if(BeginAbrupt(frame,completion.kind,completion.value,completion.target,current))break;if(completion.kind==CompletionKind::Return)return {false,completion.value};frame->ip=completion.target;break;}
            case Op::Pop:pop();break;case Op::Duplicate:if(!stack.empty())stack.push_back(stack.back());break;
            case Op::Add:{auto b=Deref(pop()),a=Deref(pop());if(a.type==Value::Type::String||b.type==Value::Type::String){std::wstring text;if(a.type==Value::Type::String)text=std::move(a.string);else text=String(a);if(b.type==Value::Type::String)text+=b.string;else text+=String(b);stack.push_back(Value::String(std::move(text)));}else stack.push_back(Value::Number(Number(a)+Number(b)));break;}
            case Op::Subtract:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)-Number(b)));break;}case Op::Multiply:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)*Number(b)));break;}case Op::Divide:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)/Number(b)));break;}case Op::Modulo:{auto b=pop(),a=pop();stack.push_back(Value::Number(std::fmod(Number(a),Number(b))));break;}case Op::Power:{auto b=pop(),a=pop();stack.push_back(Value::Number(std::pow(Number(a),Number(b))));break;}
            case Op::BitwiseAnd:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)&Uint32(b))));break;}
            case Op::BitwiseOr:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)|Uint32(b))));break;}
            case Op::BitwiseXor:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)^Uint32(b))));break;}
            case Op::ShiftLeft:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)<<(Uint32(b)&31u))));break;}
            case Op::ShiftRight:{auto b=pop(),a=pop();stack.push_back(Value::Number(Int32(a)>>(Uint32(b)&31u)));break;}
            case Op::UnsignedShiftRight:{auto b=pop(),a=pop();stack.push_back(Value::Number(Uint32(a)>>(Uint32(b)&31u)));break;}
            case Op::InValue:{auto object=pop(),key=pop();stack.push_back(Value::Bool(HasProperty(object,String(key))));break;}
            case Op::Equal:{auto b=pop(),a=pop();stack.push_back(Value::Bool(LooseEqualValues(a,b)));break;}case Op::NotEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(!LooseEqualValues(a,b)));break;}case Op::StrictEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(EqualValues(a,b)));break;}case Op::StrictNotEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(!EqualValues(a,b)));break;}
            case Op::Less:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)<Number(b)));break;}case Op::LessEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)<=Number(b)));break;}case Op::Greater:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)>Number(b)));break;}case Op::GreaterEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)>=Number(b)));break;}
            case Op::Not:stack.push_back(Value::Bool(!Truth(pop())));break;case Op::BitwiseNot:stack.push_back(Value::Number(static_cast<std::int32_t>(~Uint32(pop()))));break;case Op::Negate:stack.push_back(Value::Number(-Number(pop())));break;case Op::Positive:stack.push_back(Value::Number(Number(pop())));break;case Op::VoidValue:pop();stack.push_back(Value::Undefined());break;case Op::TypeOf:stack.push_back(Value::String(TypeName(pop())));break;
            case Op::Jump:{const auto target=static_cast<size_t>(ins.argument);if(!BeginAbrupt(frame,CompletionKind::Jump,Value::Undefined(),target,current))frame->ip=target;break;}
            case Op::JumpFalse:if(!Truth(pop()))frame->ip=static_cast<size_t>(ins.argument);break;
            case Op::JumpFalseKeep:if(!Truth(stack.back()))frame->ip=static_cast<size_t>(ins.argument);else pop();break;case Op::JumpTrueKeep:if(Truth(stack.back()))frame->ip=static_cast<size_t>(ins.argument);else pop();break;
            case Op::JumpNotNullishKeep:{const auto value=Deref(stack.back());if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null)frame->ip=static_cast<size_t>(ins.argument);else pop();break;}
            case Op::Template:stack.push_back(EvaluateTemplate(frame->chunk->constants[ins.argument].string,frame->env));break;
            case Op::Return:{const auto value=stack.empty()?Value::Undefined():Deref(pop());if(BeginAbrupt(frame,CompletionKind::Return,value,0,current))break;return {false,value};}
            }}catch(const JavaScriptException& exception){if(!DispatchException(frame,exception.value,current))throw;}
            catch(const std::exception& exception){const auto error=ErrorValue(L"Error",Utf8ToWide(exception.what()));if(!DispatchException(frame,error,current))throw JavaScriptException{error};}
        }
        return {false,Value::Undefined()};
    }
    Value StartAsyncFunction(const std::shared_ptr<Prototype>& prototype,const std::shared_ptr<Environment>& env){
        auto promise=PromiseValue();auto frame=std::make_shared<ExecutionFrame>();frame->chunk=&prototype->chunk;frame->prototype=prototype;frame->env=env;frame->asyncPromise=promise.object;
        try{const auto result=RunFrame(frame);if(!result.suspended)ResolvePromise(promise.object,result.value);}
        catch(const JavaScriptException& exception){RejectPromise(promise.object,exception.value);}
        catch(const std::exception& exception){RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
        return promise;
    }
    Value Run(const Chunk& chunk,const std::shared_ptr<Environment>& env){
        auto frame=std::make_shared<ExecutionFrame>();frame->chunk=&chunk;frame->env=env;
        frame->stack.reserve(std::min<size_t>(chunk.code.size(),64));
        const auto result=RunFrame(frame);if(result.suspended)throw std::runtime_error("await outside async execution frame");return result.value;
    }
    void InstallGlobals(){
        global->values[L"document"]=ObjectValue(ObjectKind::Document);
        global->values[L"Infinity"]=Value::Number(std::numeric_limits<double>::infinity());
        global->values[L"NaN"]=Value::Number(std::numeric_limits<double>::quiet_NaN());
        auto window=CreateObject(ObjectKind::Window);auto chrome=CreateObject(ObjectKind::Plain);const auto hostBridge=ObjectValue(ObjectKind::WebView);chrome->props[L"webview"]=hostBridge;window->props[L"chrome"]=Value::FromObject(chrome);window->props[L"twebframe"]=hostBridge;global->values[L"window"]=Value::FromObject(window);
        const auto getSelection=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.SelectionValue();});
        global->values[L"getSelection"]=getSelection;window->props[L"getSelection"]=getSelection;
        auto nodeConstants=ObjectValue(ObjectKind::Plain);nodeConstants.object->props[L"ELEMENT_NODE"]=Value::Number(1);
        nodeConstants.object->props[L"TEXT_NODE"]=Value::Number(3);nodeConstants.object->props[L"DOCUMENT_NODE"]=Value::Number(9);nodeConstants.object->props[L"DOCUMENT_FRAGMENT_NODE"]=Value::Number(11);
        global->values[L"Node"]=nodeConstants;window->props[L"Node"]=nodeConstants;
        auto nodeFilter=ObjectValue(ObjectKind::Plain);nodeFilter.object->props[L"SHOW_ALL"]=Value::Number(0xffffffffu);nodeFilter.object->props[L"SHOW_ELEMENT"]=Value::Number(1);nodeFilter.object->props[L"SHOW_TEXT"]=Value::Number(4);global->values[L"NodeFilter"]=nodeFilter;window->props[L"NodeFilter"]=nodeFilter;
        const auto eventConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto event=r.CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(a.empty()?L"":r.String(a[0]));
            event->props[L"detail"]=Value::Null();event->props[L"bubbles"]=Value::Bool(false);
            event->props[L"cancelable"]=Value::Bool(false);event->props[L"defaultPrevented"]=Value::Bool(false);
            event->props[L"data"]=Value::String(L"");event->props[L"inputType"]=Value::String(L"");
            event->props[L"isTrusted"]=Value::Bool(false);
            if(a.size()>1){const auto options=r.Deref(a[1]);if(options.type==Value::Type::Object&&options.object){
                const auto detail=options.object->props.find(L"detail");if(detail!=options.object->props.end())event->props[L"detail"]=r.Deref(detail->second);
                event->props[L"bubbles"]=Value::Bool(r.Truth(r.GetProperty(options,L"bubbles")));
                event->props[L"cancelable"]=Value::Bool(r.Truth(r.GetProperty(options,L"cancelable")));
                const auto data=options.object->props.find(L"data");if(data!=options.object->props.end())event->props[L"data"]=r.Deref(data->second);
                const auto inputType=options.object->props.find(L"inputType");if(inputType!=options.object->props.end())event->props[L"inputType"]=r.Deref(inputType->second);
            }}
            return Value::FromObject(event);
        });
        global->values[L"Event"]=eventConstructor;global->values[L"CustomEvent"]=eventConstructor;global->values[L"InputEvent"]=eventConstructor;
        window->props[L"Event"]=eventConstructor;window->props[L"CustomEvent"]=eventConstructor;window->props[L"InputEvent"]=eventConstructor;
        const auto alert=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(r.dialogSink)r.dialogSink(a.empty()?L"undefined":r.String(a[0]));
            return Value::Undefined();
        });
        global->values[L"alert"]=alert;window->props[L"alert"]=alert;
        const auto innerWidth=Value::Number(viewportWidth),innerHeight=Value::Number(viewportHeight);
        global->values[L"innerWidth"]=innerWidth;global->values[L"innerHeight"]=innerHeight;
        window->props[L"innerWidth"]=innerWidth;window->props[L"innerHeight"]=innerHeight;
        const auto pixelRatio=Value::Number(devicePixelRatio);
        global->values[L"devicePixelRatio"]=pixelRatio;window->props[L"devicePixelRatio"]=pixelRatio;
        auto parent=parentMessageSink?ObjectValue(ObjectKind::FrameWindow):Value::FromObject(window);
        global->values[L"parent"]=parent;window->props[L"parent"]=parent;
        global->values[L"performance"]=ObjectValue(ObjectKind::Performance);global->values[L"Math"]=ObjectValue(ObjectKind::Math);global->values[L"JSON"]=ObjectValue(ObjectKind::Json);global->values[L"Array"]=ObjectValue(ObjectKind::ArrayConstructor);
        auto objectConstructor=CreateObject(ObjectKind::ObjectConstructor);auto objectPrototype=CreateObject(ObjectKind::Plain);objectPrototype->props[L"hasOwnProperty"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){const auto object=r.Deref(thisValue);return Value::Bool(!a.empty()&&object.type==Value::Type::Object&&object.object&&object.object->props.count(r.String(a[0]))!=0);});objectConstructor->props[L"prototype"]=Value::FromObject(objectPrototype);global->values[L"Object"]=Value::FromObject(objectConstructor);
        global->values[L"Number"]=ObjectValue(ObjectKind::NumberConstructor);
        global->values[L"String"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::String(a.empty()?L"":r.String(a[0]));});
        global->values[L"RegExp"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto expression=r.ObjectValue(ObjectKind::RegExp);
            const auto pattern=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(pattern.type==Value::Type::Object&&pattern.object&&pattern.object->kind==ObjectKind::RegExp){
                expression.object->props[L"$pattern"]=pattern.object->props[L"$pattern"];
                expression.object->props[L"$flags"]=a.size()>1?Value::String(r.String(a[1])):pattern.object->props[L"$flags"];
            }else{
                expression.object->props[L"$pattern"]=Value::String(a.empty()?L"":r.String(a[0]));
                expression.object->props[L"$flags"]=Value::String(a.size()>1?r.String(a[1]):L"");
            }
            return expression;
        });
        global->values[L"encodeURIComponent"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto bytes=WideToUtf8(a.empty()?L"undefined":r.String(a[0]));
            constexpr wchar_t hex[]=L"0123456789ABCDEF";
            std::wstring encoded;
            for(unsigned char byte:bytes){
                if((byte>=L'A'&&byte<=L'Z')||(byte>=L'a'&&byte<=L'z')||
                   (byte>=L'0'&&byte<=L'9')||byte=='-'||byte=='_'||byte=='.'||
                   byte=='!'||byte=='~'||byte=='*'||byte=='\''||byte=='('||byte==')')
                    encoded+=static_cast<wchar_t>(byte);
                else{encoded+=L'%';encoded+=hex[byte>>4];encoded+=hex[byte&15];}
            }
            return Value::String(encoded);
        });
        global->values[L"decodeURIComponent"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto encoded=a.empty()?L"undefined":r.String(a[0]);std::string bytes;
            for(size_t index=0;index<encoded.size();){
                if(encoded[index]==L'%'){
                    if(index+2>=encoded.size())throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                    const int high=HexDigitValue(encoded[index+1]),low=HexDigitValue(encoded[index+2]);
                    if(high<0||low<0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                    bytes.push_back(static_cast<char>((high<<4)|low));index+=3;continue;
                }
                const auto next=encoded.find(L'%',index);const auto chunk=encoded.substr(index,next==std::wstring::npos?std::wstring::npos:next-index);
                bytes+=WideToUtf8(chunk);if(next==std::wstring::npos)break;index=next;
            }
            if(bytes.empty())return Value::String(L"");
            const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
            if(count<=0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
            std::wstring decoded(static_cast<size_t>(count),L'\0');
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),decoded.data(),count);
            return Value::String(std::move(decoded));
        });
        global->values[L"Boolean"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&r.Truth(a[0]));});
        global->values[L"BigInt"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Number(a.empty()?0:r.Number(a[0]));});
        for(const auto* name:{L"Error",L"TypeError",L"RangeError",L"ReferenceError",L"SyntaxError",L"AggregateError",L"URIError"}){auto constructor=ObjectValue(ObjectKind::ErrorConstructor);constructor.object->props[L"$name"]=Value::String(name);global->values[name]=constructor;}
        global->values[L"parseFloat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(std::numeric_limits<double>::quiet_NaN());const auto text=Trim(r.String(a[0]));if(text.empty())return Value::Number(std::numeric_limits<double>::quiet_NaN());wchar_t* end=nullptr;const double number=std::wcstod(text.c_str(),&end);return Value::Number(end==text.c_str()?std::numeric_limits<double>::quiet_NaN():number);});
        global->values[L"Map"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ObjectValue(ObjectKind::Map);});
        global->values[L"Set"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ObjectValue(ObjectKind::Set);});
        const auto fileReader=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ObjectValue(ObjectKind::FileReader);});
        global->values[L"FileReader"]=fileReader;window->props[L"FileReader"]=fileReader;
        auto navigator=ObjectValue(ObjectKind::Plain);auto clipboard=ObjectValue(ObjectKind::Clipboard);navigator.object->props[L"clipboard"]=clipboard;
        global->values[L"navigator"]=navigator;window->props[L"navigator"]=navigator;
        global->values[L"Date"]=ObjectValue(ObjectKind::DateConstructor);
        auto intl=ObjectValue(ObjectKind::Plain);
        intl.object->props[L"RelativeTimeFormat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto locale=a.empty()?L"":ToLower(r.String(a[0]));auto formatter=r.ObjectValue(ObjectKind::Plain);
            formatter.object->props[L"format"]=r.Native([locale](RuntimeCore& inner,const Value&,const std::vector<Value>& values){
                const double raw=values.empty()?0:inner.Number(values[0]);const auto unit=values.size()>1?ToLower(inner.String(values[1])):L"second";const auto amount=NumberString(std::abs(raw));
                if(locale.rfind(L"ko",0)==0){std::wstring translated=unit;if(unit==L"second")translated=L"\uCD08";else if(unit==L"minute")translated=L"\uBD84";else if(unit==L"hour")translated=L"\uC2DC\uAC04";else if(unit==L"day")translated=L"\uC77C";else if(unit==L"week")translated=L"\uC8FC";else if(unit==L"month")translated=L"\uAC1C\uC6D4";else if(unit==L"year")translated=L"\uB144";return Value::String(amount+translated+(raw<0?L" \uC804":L" \uD6C4"));}
                auto label=unit;if(std::abs(raw)!=1)label+=L"s";return Value::String(raw<0?amount+L" "+label+L" ago":L"in "+amount+L" "+label);
            });return formatter;
        });
        intl.object->props[L"DateTimeFormat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto locale=a.empty()?L"":r.String(a[0]);
            std::wstring year,month,day,hour,minute,second;int hour12=-1;
            if(a.size()>1){
                const auto options=r.Deref(a[1]);
                if(options.type==Value::Type::Object&&options.object){
                    auto stringOption=[&](const wchar_t* name){
                        const auto found=options.object->props.find(name);
                        return found==options.object->props.end()?std::wstring{}:r.String(found->second);
                    };
                    year=stringOption(L"year");month=stringOption(L"month");day=stringOption(L"day");
                    hour=stringOption(L"hour");minute=stringOption(L"minute");second=stringOption(L"second");
                    if(const auto found=options.object->props.find(L"hour12");found!=options.object->props.end()){
                        const auto value=r.Deref(found->second);
                        if(value.type==Value::Type::Boolean)hour12=value.boolean?1:0;
                    }
                }
            }
            auto formatter=r.ObjectValue(ObjectKind::Plain);
            formatter.object->props[L"format"]=r.Native([locale,year,month,day,hour,minute,second,hour12](RuntimeCore& inner,const Value&,const std::vector<Value>& values){
                const auto value=values.empty()?inner.DateValue({}):inner.Deref(values[0]);const double milliseconds=value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Date?inner.Number(value.object->props[L"$time"]):inner.Number(value);SYSTEMTIME time{};if(!DateSystemTime(milliseconds,time,true))return Value::String(L"Invalid Date");
                wchar_t date[128]{},clock[128]{};const auto name=locale.empty()?LOCALE_NAME_USER_DEFAULT:locale.c_str();
                const bool wantsDate=!year.empty()||!month.empty()||!day.empty();
                const bool wantsTime=!hour.empty()||!minute.empty()||!second.empty();
                if(wantsDate){
                    if(year.empty()&&!month.empty()&&!day.empty()){
                        wchar_t pattern[128]{};
                        if(GetLocaleInfoEx(name,LOCALE_SMONTHDAY,pattern,
                                           static_cast<int>(std::size(pattern)))>0)
                            GetDateFormatEx(name,0,&time,pattern,date,
                                            static_cast<int>(std::size(date)),nullptr);
                    }else GetDateFormatEx(name,DATE_SHORTDATE,&time,nullptr,date,
                                          static_cast<int>(std::size(date)),nullptr);
                }
                if(wantsTime){
                    DWORD flags=second.empty()?TIME_NOSECONDS:0;
                    if(hour12==0)flags|=TIME_FORCE24HOURFORMAT|TIME_NOTIMEMARKER;
                    GetTimeFormatEx(name,flags,&time,nullptr,clock,static_cast<int>(std::size(clock)));
                }
                if(date[0]&&clock[0])return Value::String(std::wstring(date)+L" "+clock);
                if(date[0])return Value::String(date);if(clock[0])return Value::String(clock);
                wchar_t fallback[40]{};swprintf_s(fallback,L"%04u-%02u-%02u %02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);return Value::String(fallback);
            });return formatter;
        });global->values[L"Intl"]=intl;
        auto css=ObjectValue(ObjectKind::Plain);css.object->props[L"escape"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto value=a.empty()?L"undefined":r.String(a[0]);std::wstring escaped;
            for(size_t index=0;index<value.size();++index){const wchar_t c=value[index];
                const bool control=(c>=1&&c<=31)||c==127;
                const bool leadingDigit=index==0&&std::iswdigit(c);
                const bool secondDigit=index==1&&value[0]==L'-'&&std::iswdigit(c);
                if(c==0){escaped+=static_cast<wchar_t>(0xfffd);continue;}
                if(control||leadingDigit||secondDigit){wchar_t buffer[16]{};swprintf_s(buffer,L"\\%x ",static_cast<unsigned>(c));escaped+=buffer;continue;}
                if(index==0&&c==L'-'&&value.size()==1){escaped+=L"\\-";continue;}
                if(c>=128||c==L'-'||c==L'_'||std::iswalnum(c)){escaped+=c;continue;}
                escaped+=L'\\';escaped+=c;
            }
            return Value::String(escaped);
        });global->values[L"CSS"]=css;
        global->values[L"requestAnimationFrame"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Number(a.empty()?0:r.ScheduleAnimationFrame(a[0]));});
        global->values[L"cancelAnimationFrame"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(!a.empty()){const unsigned id=static_cast<unsigned>(r.Number(a[0]));
                r.frameCallbacks.erase(std::remove_if(r.frameCallbacks.begin(),r.frameCallbacks.end(),
                    [id](const auto& frame){return frame.first==id;}),r.frameCallbacks.end());}
            return Value::Undefined();
        });
        auto timeout=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(0);return Value::Number(r.ScheduleTimer(a[0],a.size()>1?r.Number(a[1]):0));});
        auto interval=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(0);return Value::Number(r.ScheduleTimer(a[0],a.size()>1?r.Number(a[1]):0,true));});
        auto clearTimeout=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())r.ClearTimer(static_cast<unsigned>(r.Number(a[0])));return Value::Undefined();});
        global->values[L"setTimeout"]=timeout;global->values[L"clearTimeout"]=clearTimeout;global->values[L"setInterval"]=interval;global->values[L"clearInterval"]=clearTimeout;window->props[L"setTimeout"]=timeout;window->props[L"clearTimeout"]=clearTimeout;window->props[L"setInterval"]=interval;window->props[L"clearInterval"]=clearTimeout;
        global->values[L"structuredClone"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return a.empty()?Value::Undefined():r.CloneValue(a[0]);});
        global->values[L"getComputedStyle"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return r.ObjectValue(ObjectKind::Style);const auto value=r.Deref(a[0]);if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node){auto style=r.ObjectValue(ObjectKind::Style);style.object->node=value.object->node;style.object->props[L"$computed"]=Value::Bool(true);return style;}return r.ObjectValue(ObjectKind::Style);});
        auto promise=ObjectValue(ObjectKind::PromiseConstructor);
        promise.object->props[L"resolve"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PromiseResolveValue(a.empty()?Value::Undefined():a[0]);});
        promise.object->props[L"reject"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto result=r.PromiseValue();r.RejectPromise(result.object,a.empty()?Value::Undefined():a[0]);return result;});
        promise.object->props[L"all"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.all requires an array"));return result;}
            auto values=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(values->size());
            if(values->empty()){r.ResolvePromise(result.object,r.ArrayValue({}));return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){(*values)[index]=args.empty()?Value::Undefined():args[0];if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});auto rejected=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.RejectPromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        promise.object->props[L"race"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.race requires an array"));return result;}
            for(const auto& value:input.object->items){const auto item=r.PromiseResolveValue(value);auto fulfilled=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.ResolvePromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});auto rejected=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.RejectPromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}return result;
        });
        promise.object->props[L"allSettled"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.allSettled requires an array"));return result;}
            auto values=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(values->size());
            if(values->empty()){r.ResolvePromise(result.object,r.ArrayValue({}));return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){auto record=runtime.ObjectValue(ObjectKind::Plain);record.object->props[L"status"]=Value::String(L"fulfilled");record.object->props[L"value"]=args.empty()?Value::Undefined():args[0];(*values)[index]=record;if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});auto rejected=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){auto record=runtime.ObjectValue(ObjectKind::Plain);record.object->props[L"status"]=Value::String(L"rejected");record.object->props[L"reason"]=args.empty()?Value::Undefined():args[0];(*values)[index]=record;if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        promise.object->props[L"any"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.any requires an array"));return result;}
            auto errors=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(errors->size());
            auto rejectAll=[result=result.object,errors](RuntimeCore& runtime){auto error=runtime.ErrorValue(L"AggregateError",L"All promises were rejected");error.object->props[L"errors"]=runtime.ArrayValue(*errors);runtime.RejectPromise(result,error);};
            if(errors->empty()){rejectAll(r);return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.ResolvePromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});auto rejected=r.Native([result=result.object,errors,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){(*errors)[index]=args.empty()?Value::Undefined():args[0];if(--*remaining==0){auto error=runtime.ErrorValue(L"AggregateError",L"All promises were rejected");error.object->props[L"errors"]=runtime.ArrayValue(*errors);runtime.RejectPromise(result,error);}return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        global->values[L"Promise"]=promise;
        global->values[L"queueMicrotask"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty()||!r.IsCallable(a[0]))throw JavaScriptException{r.ErrorValue(L"TypeError",L"queueMicrotask callback is not callable")};const auto callback=r.Deref(a[0]);auto* runtime=&r;r.EnqueueMicrotask([runtime,callback](){runtime->Call(callback,Value::Undefined(),{});});return Value::Undefined();});
        auto locationObject=CreateObject(ObjectKind::Location);const auto query=location.find(L'?');const auto fragment=location.find(L'#');
        const bool hasQuery=query!=std::wstring::npos&&(fragment==std::wstring::npos||query<fragment);
        const auto pathEnd=std::min(hasQuery?query:location.size(),fragment==std::wstring::npos?location.size():fragment);
        locationObject->props[L"href"]=Value::String(location);
        locationObject->props[L"pathname"]=Value::String(location.substr(0,pathEnd));
        locationObject->props[L"search"]=Value::String(hasQuery?location.substr(query,(fragment==std::wstring::npos?location.size():fragment)-query):L"");
        locationObject->props[L"hash"]=Value::String(fragment==std::wstring::npos?L"":location.substr(fragment));
        global->values[L"location"]=Value::FromObject(locationObject);window->props[L"location"]=Value::FromObject(locationObject);
        auto storage=ObjectValue(ObjectKind::Storage);global->values[L"localStorage"]=storage;window->props[L"localStorage"]=storage;
        global->values[L"URLSearchParams"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto parameters=r.ObjectValue(ObjectKind::UrlSearchParams);auto query=a.empty()?L"":r.String(a[0]);if(!query.empty()&&query.front()==L'?')query.erase(query.begin());size_t start=0;while(start<=query.size()){const auto amp=query.find(L'&',start);const auto part=query.substr(start,amp==std::wstring::npos?std::wstring::npos:amp-start);const auto equal=part.find(L'=');if(!part.empty())parameters.object->props[part.substr(0,equal)]=Value::String(equal==std::wstring::npos?L"":part.substr(equal+1));if(amp==std::wstring::npos)break;start=amp+1;}return parameters;});
        global->values[L"matchMedia"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){auto media=r.ObjectValue(ObjectKind::MediaQuery);media.object->props[L"matches"]=Value::Bool(false);return media;});
        auto console=ObjectValue(ObjectKind::Plain);
        for(const auto* level:{L"log",L"info",L"warn",L"error",L"debug"}){
            const std::wstring name=level;
            console.object->props[name]=Native([name](RuntimeCore& runtime,const Value&,const std::vector<Value>& arguments){
                std::wstring line=L"[TWebFrame console."+name+L"]";
                for(const auto& argument:arguments)line+=L" "+runtime.String(argument);
                line+=L"\r\n";OutputDebugStringW(line.c_str());return Value::Undefined();
            });
        }
        global->values[L"console"]=console;
        global->values[L"fetch"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto resource=a.empty()?L"":r.String(a[0]);std::wstring body;const bool loaded=r.resourceLoader&&r.resourceLoader(resource,body);auto response=r.ObjectValue(ObjectKind::Response);response.object->props[L"ok"]=Value::Bool(loaded);response.object->props[L"status"]=Value::Number(loaded?200:404);response.object->props[L"$body"]=Value::String(std::move(body));return r.PromiseResolveValue(response);});
    }
    bool CompileRun(const std::wstring& source,std::wstring* result,std::wstring* error){
        MutationBatch batch(*this);
        try{Compiler compiler(module,source);auto value=Run(compiler.CompileProgram(),global);if(result)*result=String(value);DrainMicrotasks();if(error)error->clear();lastError.clear();return true;}
        catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;}
        catch(const std::exception& e){const auto* text=e.what();const int bytes=static_cast<int>(std::strlen(text));int count=MultiByteToWideChar(CP_UTF8,0,text,bytes,nullptr,0);lastError.assign(std::max(0,count),L'\0');if(count>0)MultiByteToWideChar(CP_UTF8,0,text,bytes,lastError.data(),count);if(error)*error=lastError;return false;}
    }
    void DispatchWebMessageValue(const Value& data){
        MutationBatch batch(*this);
        auto event=CreateObject(ObjectKind::Event);event->props[L"data"]=Deref(data);event->props[L"type"]=Value::String(L"message");
        auto eventValue=Value::FromObject(event);global->values[L"event"]=eventValue;
        const auto target=GetProperty(global->values[L"window"],L"twebframe");
        event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        InvokeEventListeners(webViewListeners,L"message",true,target,eventValue,event);
        InvokeEventListeners(webViewListeners,L"message",false,target,eventValue,event);
        DrainMicrotasks();
    }
    bool DispatchWebMessageJson(const std::wstring& json,std::wstring* error){
        try{Compiler compiler(module,json);DispatchWebMessageValue(Run(compiler.CompileExpressionOnly(),global));if(error)error->clear();lastError.clear();return true;}
        catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;}
        catch(const std::exception& e){const auto* text=e.what();const int bytes=static_cast<int>(std::strlen(text));int count=MultiByteToWideChar(CP_UTF8,0,text,bytes,nullptr,0);lastError.assign(std::max(0,count),L'\0');if(count>0)MultiByteToWideChar(CP_UTF8,0,text,bytes,lastError.data(),count);if(error)*error=lastError;return false;}
    }
    bool DispatchWindowMessageJson(const std::wstring& json,const std::shared_ptr<Node>& sourceFrame,std::wstring* error){
        try{
            Compiler compiler(module,json);
            const auto data=Run(compiler.CompileExpressionOnly(),global);
            MutationBatch batch(*this);
            auto event=CreateObject(ObjectKind::Event);
            event->props[L"data"]=Deref(data);event->props[L"type"]=Value::String(L"message");
            event->props[L"source"]=sourceFrame?FrameWindowValue(sourceFrame):global->values[L"parent"];
            const auto eventValue=Value::FromObject(event),target=global->values[L"window"];
            event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
            InvokeEventListeners(windowListeners,L"message",true,target,eventValue,event);
            InvokeEventListeners(windowListeners,L"message",false,target,eventValue,event);
            DrainMicrotasks();
            if(error)error->clear();lastError.clear();return true;
        }catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;
        }catch(const std::exception& exception){
            const auto* message=exception.what();const int bytes=static_cast<int>(std::strlen(message));
            const int count=MultiByteToWideChar(CP_UTF8,0,message,bytes,nullptr,0);
            lastError.assign(std::max(0,count),L'\0');
            if(count>0)MultiByteToWideChar(CP_UTF8,0,message,bytes,lastError.data(),count);
            if(error)*error=lastError;return false;
        }
    }
    void DispatchInlineEventHandler(const std::shared_ptr<Node>& current,
                                    const std::wstring& eventName,
                                    const Value& eventValue,
                                    const std::shared_ptr<Object>& event){
        if(!current)return;
        const auto source=current->Attribute(L"on"+ToLower(eventName));
        if(source.empty())return;
        try{
            // HTML event attributes execute as functions whose `this` value and
            // event argument are the element currently handling the event. Keep
            // this in the shared dispatcher so inline handlers and listeners see
            // the same target/currentTarget state during bubbling.
            const auto cacheKey=ToLower(eventName)+L'\x1f'+source;
            auto cached=inlineHandlerCache.find(cacheKey);
            Value callback;
            if(cached!=inlineHandlerCache.end())callback=cached->second;
            else{
                Compiler compiler(module,L"(function(event){\n"+source+L"\n})");
                callback=Run(compiler.CompileExpressionOnly(),global);
                if(inlineHandlerCache.size()>=256)inlineHandlerCache.clear();
                inlineHandlerCache.emplace(cacheKey,callback);
            }
            const auto result=Deref(Call(callback,NodeValue(current),{eventValue}));
            if(result.type==Value::Type::Boolean&&!result.boolean)
                event->props[L"defaultPrevented"]=Value::Bool(true);
        }catch(const JavaScriptException& exception){
            lastError=L"Uncaught "+String(exception.value);
        }catch(const std::exception& exception){
            lastError=Utf8ToWide(exception.what());
        }
    }
    bool Dispatch(const std::shared_ptr<Node>& node,const std::wstring& eventName,
                  const std::vector<Node::FileInfo>* transferredFiles=nullptr,
                  const JavaScriptRuntime::EventInit& init={},
                  const std::wstring* transferredText=nullptr){
        MutationBatch batch(*this);
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(eventName);
        event->props[L"target"]=NodeValue(node);event->props[L"currentTarget"]=NodeValue(node);
        event->props[L"key"]=Value::String(init.key);event->props[L"button"]=Value::Number(init.button);
        event->props[L"buttons"]=Value::Number(init.buttons);
        event->props[L"clientX"]=Value::Number(init.clientX);event->props[L"clientY"]=Value::Number(init.clientY);
        event->props[L"x"]=Value::Number(init.clientX);event->props[L"y"]=Value::Number(init.clientY);
        event->props[L"pageX"]=Value::Number(init.pageX);event->props[L"pageY"]=Value::Number(init.pageY);
        event->props[L"screenX"]=Value::Number(init.screenX);event->props[L"screenY"]=Value::Number(init.screenY);
        event->props[L"movementX"]=Value::Number(init.movementX);event->props[L"movementY"]=Value::Number(init.movementY);
        event->props[L"relatedTarget"]=NodeValue(init.relatedTarget);
        event->props[L"pointerId"]=Value::Number(1);event->props[L"pointerType"]=Value::String(L"mouse");
        event->props[L"isPrimary"]=Value::Bool(true);
        event->props[L"data"]=Value::String(init.data);event->props[L"inputType"]=Value::String(init.inputType);
        event->props[L"detail"]=Value::Number(init.detail);event->props[L"ctrlKey"]=Value::Bool(init.ctrlKey);
        event->props[L"shiftKey"]=Value::Bool(init.shiftKey);event->props[L"altKey"]=Value::Bool(init.altKey);
        event->props[L"metaKey"]=Value::Bool(init.metaKey);event->props[L"isComposing"]=Value::Bool(init.isComposing);
        event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(true);
        event->props[L"bubbles"]=Value::Bool(init.bubbles);event->props[L"cancelable"]=Value::Bool(init.cancelable);event->props[L"eventPhase"]=Value::Number(0);
        if(transferredFiles||transferredText){const std::vector<Node::FileInfo> emptyFiles;
            auto transfer=DataTransferValue(transferredText?*transferredText:L"",transferredFiles?*transferredFiles:emptyFiles);
            event->props[eventName==L"paste"||eventName==L"copy"||eventName==L"cut"?L"clipboardData":L"dataTransfer"]=transfer;}
        auto eventValue=Value::FromObject(event);global->values[L"event"]=eventValue;
        auto stopped=[&](const wchar_t* property){const auto found=event->props.find(property);return found!=event->props.end()&&Truth(found->second);};
        const auto windowTarget=global->values[L"window"],documentTarget=global->values[L"document"];
        auto invoke=[&](EventListenerMap& map,const Value& target,bool capture,int phase){event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(phase);InvokeEventListeners(map,eventName,capture,target,eventValue,event);};
        if(node){
            std::vector<std::shared_ptr<Node>> ancestors;
            for(auto current=node->parent.lock();current;current=current->parent.lock())
                if(current->type!=NodeType::Document)ancestors.push_back(current);
            invoke(windowListeners,windowTarget,true,1);
            if(!stopped(L"$propagationStopped"))invoke(documentListeners,documentTarget,true,1);
            if(!stopped(L"$propagationStopped"))for(auto iterator=ancestors.rbegin();iterator!=ancestors.rend();++iterator){
                auto found=listeners.find(iterator->get());if(found!=listeners.end())invoke(found->second.events,NodeValue(*iterator),true,1);
                if(stopped(L"$propagationStopped"))break;
            }
            if(!stopped(L"$propagationStopped")){
                const auto target=NodeValue(node);auto found=listeners.find(node.get());
                if(found!=listeners.end())invoke(found->second.events,target,true,2);
                if(!stopped(L"$immediateStopped")){event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);DispatchInlineEventHandler(node,eventName,eventValue,event);}
                if(!stopped(L"$immediateStopped")&&found!=listeners.end())invoke(found->second.events,target,false,2);
            }
            if(init.bubbles&&!stopped(L"$propagationStopped"))for(const auto& current:ancestors){
                const auto target=NodeValue(current);event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(3);
                DispatchInlineEventHandler(current,eventName,eventValue,event);
                if(!stopped(L"$immediateStopped")){auto found=listeners.find(current.get());if(found!=listeners.end())invoke(found->second.events,target,false,3);}
                if(stopped(L"$propagationStopped"))break;
            }
            if(init.bubbles&&!stopped(L"$propagationStopped"))invoke(documentListeners,documentTarget,false,3);
            if(init.bubbles&&!stopped(L"$propagationStopped"))invoke(windowListeners,windowTarget,false,3);
        }else{
            event->props[L"target"]=documentTarget;
            invoke(windowListeners,windowTarget,true,1);
            if(!stopped(L"$propagationStopped")){
                invoke(documentListeners,documentTarget,true,2);
                if(!stopped(L"$immediateStopped"))invoke(documentListeners,documentTarget,false,2);
            }
            if(!stopped(L"$propagationStopped"))invoke(windowListeners,windowTarget,false,3);
        }
        event->props[L"currentTarget"]=Value::Null();event->props[L"eventPhase"]=Value::Number(0);
        DrainMicrotasks();
        return stopped(L"defaultPrevented");
    }
    bool DispatchWindowEventObject(const std::shared_ptr<Object>& event){
        MutationBatch batch(*this);
        const auto target=global->values[L"window"],eventValue=Value::FromObject(event);
        const auto eventName=String(event->props[L"type"]);event->props[L"target"]=target;
        event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        if(!event->props.count(L"defaultPrevented"))event->props[L"defaultPrevented"]=Value::Bool(false);
        if(!event->props.count(L"isTrusted"))event->props[L"isTrusted"]=Value::Bool(false);
        InvokeEventListeners(windowListeners,eventName,true,target,eventValue,event);
        const auto immediate=event->props.find(L"$immediateStopped");
        if(immediate==event->props.end()||!Truth(immediate->second))InvokeEventListeners(windowListeners,eventName,false,target,eventValue,event);
        event->props[L"currentTarget"]=Value::Null();event->props[L"eventPhase"]=Value::Number(0);
        DrainMicrotasks();
        return !Truth(event->props[L"defaultPrevented"]);
    }
    void DispatchWindow(const std::wstring& eventName){
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(eventName);
        event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(true);
        DispatchWindowEventObject(event);
    }
};

} // namespace

struct JavaScriptRuntime::Impl {
    RuntimeCore core;
    explicit Impl(Document& document):core(document){}
};

JavaScriptRuntime::JavaScriptRuntime(Document& document):impl_(std::make_unique<Impl>(document)){}
JavaScriptRuntime::~JavaScriptRuntime()=default;
void JavaScriptRuntime::SetMessageSink(MessageSink sink){impl_->core.messageSink=std::move(sink);}
void JavaScriptRuntime::SetMutationSink(MutationSink sink){impl_->core.mutationSink=std::move(sink);}
void JavaScriptRuntime::SetFrameScheduler(FrameScheduler scheduler){impl_->core.frameScheduler=std::move(scheduler);}
void JavaScriptRuntime::SetTimerScheduler(TimerScheduler scheduler){impl_->core.timerScheduler=std::move(scheduler);}
void JavaScriptRuntime::SetGeometryProvider(GeometryProvider provider){impl_->core.geometryProvider=std::move(provider);}
void JavaScriptRuntime::SetStylePropertyProvider(StylePropertyProvider provider){impl_->core.stylePropertyProvider=std::move(provider);}
void JavaScriptRuntime::SetResourceLoader(ResourceLoader loader){impl_->core.resourceLoader=std::move(loader);}
void JavaScriptRuntime::SetDialogSink(DialogSink sink){impl_->core.dialogSink=std::move(sink);}
void JavaScriptRuntime::SetFrameMessageSink(FrameMessageSink sink){impl_->core.frameMessageSink=std::move(sink);}
void JavaScriptRuntime::SetParentMessageSink(ParentMessageSink sink){impl_->core.parentMessageSink=std::move(sink);}
void JavaScriptRuntime::SetFocusSink(FocusSink sink){impl_->core.focusSink=std::move(sink);}
void JavaScriptRuntime::SetActivationSink(ActivationSink sink){impl_->core.activationSink=std::move(sink);}
void JavaScriptRuntime::SetPointerCaptureSink(PointerCaptureSink sink){impl_->core.pointerCaptureSink=std::move(sink);}
void JavaScriptRuntime::SetSelectionProvider(SelectionProvider provider){impl_->core.selectionProvider=std::move(provider);}
void JavaScriptRuntime::SetSelectionSetter(SelectionSetter setter){impl_->core.selectionSetter=std::move(setter);}
void JavaScriptRuntime::SetDomSelectionProvider(DomSelectionProvider provider){impl_->core.domSelectionProvider=std::move(provider);}
void JavaScriptRuntime::SetDomSelectionSetter(DomSelectionSetter setter){impl_->core.domSelectionSetter=std::move(setter);}
void JavaScriptRuntime::SetViewportSize(double width,double height){
    impl_->core.viewportWidth=std::max(0.0,width);impl_->core.viewportHeight=std::max(0.0,height);
    const auto widthValue=Value::Number(impl_->core.viewportWidth),heightValue=Value::Number(impl_->core.viewportHeight);
    impl_->core.global->values[L"innerWidth"]=widthValue;impl_->core.global->values[L"innerHeight"]=heightValue;
    const auto found=impl_->core.global->values.find(L"window");
    if(found!=impl_->core.global->values.end()&&found->second.type==Value::Type::Object&&found->second.object){
        found->second.object->props[L"innerWidth"]=widthValue;
        found->second.object->props[L"innerHeight"]=heightValue;
    }
}
void JavaScriptRuntime::SetDevicePixelRatio(double ratio){
    impl_->core.devicePixelRatio=std::isfinite(ratio)&&ratio>0?ratio:1;
    const auto value=Value::Number(impl_->core.devicePixelRatio);
    impl_->core.global->values[L"devicePixelRatio"]=value;
    const auto found=impl_->core.global->values.find(L"window");
    if(found!=impl_->core.global->values.end()&&found->second.object)
        found->second.object->props[L"devicePixelRatio"]=value;
}
void JavaScriptRuntime::SetLocation(const std::wstring& location){impl_->core.location=location;}
void JavaScriptRuntime::NavigateToFragment(const std::wstring& fragment){
    const auto found=impl_->core.global->values.find(L"location");
    if(found!=impl_->core.global->values.end())
        impl_->core.SetProperty(found->second,L"hash",Value::String(fragment));
}
bool JavaScriptRuntime::Load(const std::wstring& source,std::wstring* error){
    impl_->core.ResetExecutionState(true);
    return impl_->core.CompileRun(source,nullptr,error);
}
bool JavaScriptRuntime::Execute(const std::wstring& source,std::wstring* result,std::wstring* error){return impl_->core.CompileRun(source,result,error);}
void JavaScriptRuntime::DispatchDocumentEvent(const std::wstring& eventName){impl_->core.Dispatch({},eventName);}
void JavaScriptRuntime::DispatchWindowEvent(const std::wstring& eventName){impl_->core.DispatchWindow(eventName);}
void JavaScriptRuntime::RunAnimationFrame(){impl_->core.RunAnimationFrame();}
void JavaScriptRuntime::RunTimers(){impl_->core.RunTimers();}
bool JavaScriptRuntime::DispatchNodeEvent(const std::shared_ptr<Node>& node,const std::wstring& eventName,const EventInit& init){
    const bool defaultPrevented=impl_->core.Dispatch(node,eventName,nullptr,init);
    // Primary pointer button transitions produce their compatibility mouse
    // events unless the pointer event was canceled. Keep the rule in the
    // shared DOM path so content-editable and ordinary elements behave alike.
    if(!defaultPrevented&&(eventName==L"pointerdown"||eventName==L"pointerup"))
        return impl_->core.Dispatch(node,eventName==L"pointerdown"?L"mousedown":L"mouseup",nullptr,init);
    return defaultPrevented;
}
bool JavaScriptRuntime::DispatchClipboardEvent(const std::shared_ptr<Node>& node,const std::wstring& eventName,const std::wstring& text,const std::vector<Node::FileInfo>& files){return impl_->core.Dispatch(node,eventName,&files,{},&text);}
std::shared_ptr<Node> JavaScriptRuntime::CapturedPointerTarget()const{return impl_->core.pointerCaptureNode.lock();}
void JavaScriptRuntime::ClearPointerCapture(){impl_->core.pointerCaptureNode.reset();if(impl_->core.pointerCaptureSink)impl_->core.pointerCaptureSink(false);}
void JavaScriptRuntime::DispatchFileDrop(const std::shared_ptr<Node>& node,const std::vector<Node::FileInfo>& files){impl_->core.Dispatch(node,L"drop",&files);}
bool JavaScriptRuntime::DispatchWebMessageAsJson(const std::wstring& json,std::wstring* error){return impl_->core.DispatchWebMessageJson(json,error);}
void JavaScriptRuntime::DispatchWebMessageAsString(const std::wstring& message){impl_->core.DispatchWebMessageValue(Value::String(message));}
bool JavaScriptRuntime::DispatchWindowMessageAsJson(const std::wstring& json,const std::shared_ptr<Node>& sourceFrame,std::wstring* error){return impl_->core.DispatchWindowMessageJson(json,sourceFrame,error);}
void JavaScriptRuntime::Clear(){impl_->core.ResetExecutionState(true);}

} // namespace TWebFrame::Internal
