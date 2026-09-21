#include "Layout.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <wrl/client.h>

namespace TWebFrame::Internal {
namespace {

struct Edges { float top=0,right=0,bottom=0,left=0; };
struct CachedStyleMetrics {
    std::weak_ptr<ComputedStyle::ValueMap> owner;
    Edges borders;
    float borderScale=0;
    float fontSize=0;
    float lineHeight=0;
    bool bordersValid=false;
    bool fontSizeValid=false;
    bool lineHeightValid=false;
};
CachedStyleMetrics& StyleMetrics(const ComputedStyle& style){
    static thread_local FastMap<const void*,CachedStyleMetrics> cache;
    const auto key=static_cast<const void*>(style.values.get());
    auto found=cache.find(key);
    if(found!=cache.end()){
        const auto owner=found->second.owner.lock();
        if(owner&&owner.get()==style.values.get())return found->second;
        cache.clear();
    }else if(cache.size()>=4096)cache.clear();
    auto& result=cache[key];result.owner=style.values;return result;
}

struct EdgeCacheKey {
    const void* values=nullptr;
    float reference=0;
    float viewport=0;
    bool padding=false;
    bool operator==(const EdgeCacheKey& other) const {
        return values==other.values&&reference==other.reference&&
            viewport==other.viewport&&padding==other.padding;
    }
};
struct EdgeCacheHash {
    size_t operator()(const EdgeCacheKey& key) const {
        size_t hash=std::hash<const void*>{}(key.values);
        hash^=std::hash<float>{}(key.reference)+0x9e3779b9u+(hash<<6)+(hash>>2);
        hash^=std::hash<float>{}(key.viewport)+0x9e3779b9u+(hash<<6)+(hash>>2);
        return hash^(key.padding?0x85ebca6bu:0u);
    }
};
struct CachedEdges {
    std::weak_ptr<ComputedStyle::ValueMap> owner;
    Edges value;
};
struct CornerRadii { float x=0,y=0; };
struct BoxShadow {
    float offsetX=0,offsetY=0,blur=0,spread=0;
    unsigned int color=0;
    bool inset=false;
};
struct VerticalScrollbarGeometry {
    LayoutRect track;
    LayoutRect thumb;
    float trackStart = 0;
    float travel = 0;
    float maximum = 0;
    float arrowHeight = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

struct VerticalScrollbarMetrics {
    float width = 15.0f;
    float arrowHeight = 0;
    float minimumThumbHeight = 0;
    float thumbInset = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

Edges BorderValues(const ComputedStyle& style);

LayoutRect ScrollbarPaddingBox(const LayoutBox& box) {
    const auto border=BorderValues(box.style);
    return {box.rect.x+border.left,box.rect.y+border.top,
        std::max(0.0f,box.rect.width-border.left-border.right),
        std::max(0.0f,box.rect.height-border.top-border.bottom)};
}

VerticalScrollbarMetrics VerticalScrollbarMetricsFor(const LayoutBox& box,const StyleSheet& styleSheet) {
    VerticalScrollbarMetrics metrics;
    const auto width=ToLower(Trim(box.style.Get(L"scrollbar-width",L"auto")));
    if(width==L"none"){metrics.width=0;metrics.arrowHeight=0;return metrics;}
    const auto standardColors=ToLower(Trim(box.style.Get(L"scrollbar-color")));
    metrics.standardStyling=width!=L"auto"||
        (!standardColors.empty()&&standardColors!=L"auto");
    const auto scrollbarStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar"):ComputedStyle{};
    const auto customWidth=Trim(scrollbarStyle.Get(L"width"));
    const bool custom=!customWidth.empty()&&customWidth!=L"auto";
    if(custom)metrics.width=std::max(0.0f,StyleSheet::Length(customWidth,box.content.width,box.content.width,metrics.width));
    else if(width==L"thin")metrics.width=10.0f;
    metrics.compactArrows=width==L"thin"||custom;
    // Keep scrollbar metrics in CSS DIPs. Direct2D applies the window DPI, so
    // the authored 10px width becomes 15 physical pixels at 150% without any
    // monitor-specific constants here. Button, thumb and arrow proportions are
    // derived from that authored width for the same reason.
    metrics.arrowHeight=metrics.width*1.2f;
    metrics.minimumThumbHeight=metrics.width*(metrics.compactArrows?3.6f:1.15f);
    metrics.thumbInset=metrics.width*0.2f;
    const auto thumbStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar-thumb")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
    const auto minimum=Trim(thumbStyle.Get(L"min-height"));
    if(!metrics.standardStyling&&!minimum.empty())
        metrics.minimumThumbHeight=std::max(0.0f,StyleSheet::Length(minimum,box.content.height,box.content.height,metrics.minimumThumbHeight));
    const auto border=Trim(thumbStyle.Get(L"border-width",thumbStyle.Get(L"border")));
    if(!metrics.standardStyling&&!border.empty())
        metrics.thumbInset=std::max(0.0f,StyleSheet::Length(border,metrics.width,metrics.width,metrics.thumbInset));
    return metrics;
}

bool VerticalScrollbarFor(const LayoutBox& box,const StyleSheet& styleSheet,VerticalScrollbarGeometry& geometry) {
    const auto overflowY=box.style.Get(L"overflow-y",L"visible");
    if((overflowY!=L"auto"&&overflowY!=L"scroll")||box.children.empty()||
       box.scrollHeight<=box.content.height+1)return false;
    const auto metrics=VerticalScrollbarMetricsFor(box,styleSheet);
    const float trackWidth=metrics.width;
    if(trackWidth<=0)return false;
    const auto paddingBox=ScrollbarPaddingBox(box);
    const float arrowHeight=metrics.arrowHeight;
    const float minimumThumbHeight=metrics.minimumThumbHeight;
    const float available=std::max(0.0f,paddingBox.height-2*arrowHeight);
    if(available<=0)return false;
    const float paddingHeight=std::max(0.0f,paddingBox.height-box.content.height);
    const float scrollExtent=box.scrollHeight+paddingHeight;
    const float thumbHeight=std::min(available,std::max(minimumThumbHeight,
        available*paddingBox.height/std::max(paddingBox.height,scrollExtent)));
    const float maximum=std::max(0.0f,box.scrollHeight-box.content.height);
    const float travel=std::max(0.0f,available-thumbHeight);
    const float trackStart=paddingBox.y+arrowHeight;
    const float thumbY=trackStart+(maximum>0?travel*(box.node->scrollTop/maximum):0);
    geometry.track={paddingBox.x+paddingBox.width-trackWidth,paddingBox.y,trackWidth,paddingBox.height};
    const float thumbInset=std::min(trackWidth/2.0f,metrics.thumbInset);
    geometry.thumb={geometry.track.x+thumbInset,thumbY,std::max(1.0f,trackWidth-2*thumbInset),thumbHeight};
    geometry.trackStart=trackStart;geometry.travel=travel;geometry.maximum=maximum;geometry.arrowHeight=arrowHeight;geometry.compactArrows=metrics.compactArrows;geometry.standardStyling=metrics.standardStyling;
    return true;
}

bool IsInlineLevel(const std::wstring& display) {
    return display==L"inline"||display==L"inline-block"||
           display==L"inline-flex"||display==L"inline-grid";
}

bool HasStableScrollbarGutter(const ComputedStyle& style) {
    std::wistringstream tokens(ToLower(style.Get(L"scrollbar-gutter")));
    std::wstring token;
    while(tokens>>token)if(token==L"stable")return true;
    return false;
}

bool IsBlockifiedItem(const LayoutBox& box) {
    if(!box.parent||box.style.Is(L"position",L"absolute")||box.style.Is(L"position",L"fixed"))return false;
    const auto parentDisplay=box.parent->style.Get(L"display");
    return parentDisplay==L"grid"||parentDisplay==L"inline-grid"||
           parentDisplay==L"flex"||parentDisplay==L"inline-flex";
}

int ZIndex(const LayoutBox& box) {
    const auto value=Trim(ToLower(box.style.Get(L"z-index",L"auto")));
    if(value.empty()||value==L"auto")return 0;
    try{return std::stoi(value);}catch(...){return 0;}
}

bool HasExplicitZIndex(const LayoutBox& box) {
    const auto value=Trim(ToLower(box.style.Get(L"z-index",L"auto")));
    return !value.empty()&&value!=L"auto";
}

bool IsStackingContext(const LayoutBox& box) {
    if(!box.parent)return true;
    const auto position=box.style.Get(L"position",L"static");
    if(position==L"fixed"||position==L"sticky")return true;
    const auto parentDisplay=box.parent->style.Get(L"display");
    const bool positioned=position!=L"static";
    const bool flexOrGridItem=parentDisplay==L"flex"||parentDisplay==L"inline-flex"||
        parentDisplay==L"grid"||parentDisplay==L"inline-grid";
    if(HasExplicitZIndex(box)&&(positioned||flexOrGridItem))return true;
    if(box.style.Get(L"transform",L"none")!=L"none")return true;
    try{return std::stof(box.style.Get(L"opacity",L"1"))<0.999f;}catch(...){return false;}
}

bool ClipsOverflow(const LayoutBox& box) {
    const auto overflow=box.style.Get(L"overflow",L"visible");
    const auto overflowX=box.style.Get(L"overflow-x",overflow);
    const auto overflowY=box.style.Get(L"overflow-y",overflow);
    const auto clips=[](const std::wstring& value){
        return value==L"hidden"||value==L"clip"||value==L"auto"||value==L"scroll";
    };
    return clips(overflow)||clips(overflowX)||clips(overflowY);
}

LayoutRect IntersectRects(const LayoutRect& left,const LayoutRect& right) {
    const float x=std::max(left.x,right.x),y=std::max(left.y,right.y);
    const float r=std::min(left.x+left.width,right.x+right.width);
    const float b=std::min(left.y+left.height,right.y+right.height);
    return {x,y,std::max(0.0f,r-x),std::max(0.0f,b-y)};
}

LayoutRect StackingContextClip(const LayoutBox& context,const LayoutBox& scope,
                               LayoutRect clip) {
    for(auto* ancestor=context.parent;ancestor&&ancestor!=&scope;ancestor=ancestor->parent)
        if(ClipsOverflow(*ancestor))clip=IntersectRects(clip,ancestor->content);
    return clip;
}

bool StackingContextAllowsPoint(const LayoutBox& context,const LayoutBox& scope,
                                float x,float y) {
    for(auto* ancestor=context.parent;ancestor&&ancestor!=&scope;ancestor=ancestor->parent)
        if(ClipsOverflow(*ancestor)&&!ancestor->content.Contains(x,y))return false;
    return true;
}

bool IsDeferredContext(const LayoutBox& box,const std::vector<LayoutBox*>* contexts) {
    return contexts&&std::find(contexts->begin(),contexts->end(),&box)!=contexts->end();
}

template<class BoxPointer>
void StableStackingOrder(std::vector<BoxPointer>& boxes) {
    std::stable_sort(boxes.begin(),boxes.end(),[](const BoxPointer left,const BoxPointer right){
        return ZIndex(*left)<ZIndex(*right);
    });
}

LayoutBox* SingleLineClippedTextChild(LayoutBox& box) {
    if(!box.style.Is(L"white-space",L"nowrap"))return nullptr;
    const auto overflow=box.style.Get(L"overflow",L"visible");
    const auto overflowX=box.style.Get(L"overflow-x",overflow);
    if(overflow!=L"hidden"&&overflowX!=L"hidden")return nullptr;

    LayoutBox* textChild=nullptr;
    for(auto& child:box.children){
        if(!child->visible)continue;
        if(textChild||child->node->type!=NodeType::Text||
           child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))
            return nullptr;
        textChild=child.get();
    }
    return textChild;
}

CornerRadii UniformCornerRadii(const ComputedStyle& style,float width,float height,float viewportWidth) {
    auto raw=Trim(ToLower(style.Get(L"border-radius",L"0")));
    if(raw.empty())return {};

    // This renderer currently paints one uniform radius. Resolve its horizontal
    // and vertical components independently, as CSS does for percentages.
    // Then apply CSS corner-overlap normalization. In particular, a large
    // length such as 999px becomes a capsule, not an ellipse whose x radius is
    // silently stretched to half the box width by the graphics backend.
    const auto slash=raw.find(L'/');
    auto first=[](std::wstring value){value=Trim(value);const auto end=value.find_first_of(L" \t\r\n");return end==std::wstring::npos?value:value.substr(0,end);};
    const auto horizontal=first(slash==std::wstring::npos?raw:raw.substr(0,slash));
    const auto vertical=slash==std::wstring::npos?horizontal:first(raw.substr(slash+1));
    CornerRadii radii{
        StyleSheet::Length(horizontal,width,viewportWidth,0),
        StyleSheet::Length(vertical,height,viewportWidth,0)};
    if(radii.x<=0||radii.y<=0)return {};
    const float scale=std::min({1.0f,width/(2*radii.x),height/(2*radii.y)});
    radii.x*=scale;radii.y*=scale;
    return radii;
}

std::vector<std::wstring> Words(const std::wstring& text) {
    std::vector<std::wstring> result; std::wstring current; int nesting=0;
    for (wchar_t c : text) {
        if (c==L'(') ++nesting; if(c==L')') --nesting;
        if (std::iswspace(c) && nesting==0) { if(!current.empty()){result.push_back(current);current.clear();} }
        else current+=c;
    }
    if(!current.empty())result.push_back(current);return result;
}

float FontSize(const ComputedStyle& style);
std::vector<float> SvgNumbers(const std::wstring& source);

std::vector<std::wstring> CommaSeparated(const std::wstring& text) {
    std::vector<std::wstring> result;size_t start=0;int nesting=0;
    for(size_t i=0;i<=text.size();++i){
        const wchar_t c=i<text.size()?text[i]:L',';
        if(c==L'(')++nesting;else if(c==L')')--nesting;
        else if(c==L','&&nesting==0){auto item=Trim(text.substr(start,i-start));if(!item.empty())result.push_back(item);start=i+1;}
    }
    return result;
}

struct TransitionDefinition {
    std::wstring property = L"all";
    float durationMs = 0;
    float delayMs = 0;
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.0f;
};

bool TransitionTime(const std::wstring& token,float& milliseconds) {
    const auto value=ToLower(Trim(token));
    try{
        size_t consumed=0;const float number=std::stof(value,&consumed);
        const auto unit=value.substr(consumed);
        if(unit==L"ms"){milliseconds=number;return true;}
        if(unit==L"s"){milliseconds=number*1000.0f;return true;}
    }catch(...){}
    return false;
}

bool TransitionTiming(const std::wstring& token,TransitionDefinition& definition) {
    const auto value=ToLower(Trim(token));
    if(value==L"linear"){definition.x1=0;definition.y1=0;definition.x2=1;definition.y2=1;return true;}
    if(value==L"ease"){definition.x1=.25f;definition.y1=.1f;definition.x2=.25f;definition.y2=1;return true;}
    if(value==L"ease-in"){definition.x1=.42f;definition.y1=0;definition.x2=1;definition.y2=1;return true;}
    if(value==L"ease-out"){definition.x1=0;definition.y1=0;definition.x2=.58f;definition.y2=1;return true;}
    if(value==L"ease-in-out"){definition.x1=.42f;definition.y1=0;definition.x2=.58f;definition.y2=1;return true;}
    if(value.rfind(L"cubic-bezier(",0)!=0||value.back()!=L')')return false;
    const auto values=CommaSeparated(value.substr(13,value.size()-14));
    if(values.size()!=4)return false;
    try{
        definition.x1=std::stof(values[0]);definition.y1=std::stof(values[1]);
        definition.x2=std::stof(values[2]);definition.y2=std::stof(values[3]);
        definition.x1=std::max(0.0f,std::min(1.0f,definition.x1));
        definition.x2=std::max(0.0f,std::min(1.0f,definition.x2));
        return true;
    }catch(...){return false;}
}

std::vector<TransitionDefinition> TransitionDefinitions(const ComputedStyle& style) {
    std::vector<TransitionDefinition> result;
    const auto shorthand=Trim(style.Get(L"transition"));
    if(shorthand.empty()||ToLower(shorthand)==L"none")return result;
    for(const auto& item:CommaSeparated(shorthand)){
        TransitionDefinition definition;bool haveDuration=false;
        for(const auto& word:Words(item)){
            float time=0;
            if(TransitionTime(word,time)){
                if(!haveDuration){definition.durationMs=std::max(0.0f,time);haveDuration=true;}
                else definition.delayMs=time;
            }else if(!TransitionTiming(word,definition))definition.property=ToLower(word);
        }
        result.push_back(std::move(definition));
    }
    return result;
}

const TransitionDefinition* TransitionFor(const std::vector<TransitionDefinition>& definitions,
                                          const std::wstring& property) {
    const TransitionDefinition* all=nullptr;const TransitionDefinition* exact=nullptr;
    for(const auto& definition:definitions){
        if(definition.property==L"all")all=&definition;
        else if(definition.property==property)exact=&definition;
    }
    return exact?exact:all;
}

struct NumericToken { size_t offset=0,length=0;double value=0; };

std::vector<NumericToken> NumericTokens(const std::wstring& value) {
    std::vector<NumericToken> result;
    for(size_t index=0;index<value.size();){
        const bool sign=(value[index]==L'+'||value[index]==L'-')&&index+1<value.size()&&
            (std::iswdigit(value[index+1])||(value[index+1]==L'.'&&index+2<value.size()&&std::iswdigit(value[index+2])));
        const bool start=std::iswdigit(value[index])||
            (value[index]==L'.'&&index+1<value.size()&&std::iswdigit(value[index+1]))||sign;
        if(!start){++index;continue;}
        wchar_t* end=nullptr;const double number=std::wcstod(value.c_str()+index,&end);
        if(end==value.c_str()+index){++index;continue;}
        const size_t length=static_cast<size_t>(end-(value.c_str()+index));
        result.push_back({index,length,number});index+=length;
    }
    return result;
}

std::wstring NumberText(double value) {
    if(std::abs(value)<0.0000005)value=0;
    std::wostringstream output;output<<std::fixed<<std::setprecision(6)<<value;
    auto text=output.str();while(text.size()>1&&text.back()==L'0')text.pop_back();
    if(!text.empty()&&text.back()==L'.')text.pop_back();return text;
}

std::wstring IdentityTransform(const std::wstring& model) {
    const auto numbers=NumericTokens(model);if(numbers.empty())return L"none";
    std::wstring result;size_t cursor=0;
    for(const auto& number:numbers){
        result.append(model,cursor,number.offset-cursor);
        const auto open=model.rfind(L'(',number.offset);
        size_t begin=open;
        while(begin!=std::wstring::npos&&begin>0&&(std::iswalpha(model[begin-1])||model[begin-1]==L'-'))--begin;
        const auto function=open==std::wstring::npos?L"":ToLower(model.substr(begin,open-begin));
        result+=function.rfind(L"scale",0)==0?L"1":L"0";
        cursor=number.offset+number.length;
    }
    result.append(model,cursor,std::wstring::npos);return result;
}

std::wstring TransitionBaseValue(const ComputedStyle& style,const std::wstring& property,
                                 const std::wstring& other=L"") {
    auto value=Trim(style.Get(property));
    if(property==L"opacity"&&value.empty())return L"1";
    if(property==L"visibility"&&value.empty())return L"visible";
    if(property==L"transform"&&(value.empty()||ToLower(value)==L"none"))
        return other.empty()?L"none":IdentityTransform(other);
    return value;
}

bool CanInterpolateNumbers(const std::wstring& from,const std::wstring& to) {
    const auto first=NumericTokens(from),second=NumericTokens(to);
    return !first.empty()&&first.size()==second.size();
}

bool IsNumericTransitionProperty(const std::wstring& property) {
    if(property==L"opacity"||property==L"transform"||property==L"grid-template-columns"||
       property==L"grid-template-rows"||property==L"gap"||property==L"row-gap"||
       property==L"column-gap"||property==L"font-size"||property==L"font-weight"||
       property==L"line-height"||property==L"letter-spacing"||property==L"word-spacing"||
       property==L"text-indent"||property==L"flex-grow"||property==L"flex-shrink"||
       property==L"flex-basis"||property==L"stroke-width"||property==L"outline-width"||
       property==L"background-position"||property==L"background-size"||
       property==L"object-position"||property==L"perspective"||property==L"z-index"||
       property==L"order")return true;
    for(const auto* prefix:{L"width",L"height",L"min-width",L"max-width",L"min-height",L"max-height",
                            L"top",L"right",L"bottom",L"left",L"margin",L"padding",L"inset",
                            L"border-radius",L"border-top-left-radius",L"border-top-right-radius",
                            L"border-bottom-left-radius",L"border-bottom-right-radius",
                            L"border-top-width",L"border-right-width",L"border-bottom-width",L"border-left-width"})
        if(property==prefix)return true;
    return false;
}

float CubicCoordinate(float t,float first,float second) {
    const float inverse=1-t;
    return 3*inverse*inverse*t*first+3*inverse*t*t*second+t*t*t;
}

float TransitionProgress(const StyleTransition& transition) {
    if(transition.elapsedMs<=transition.delayMs)return 0;
    if(transition.durationMs<=0)return 1;
    const float linear=std::max(0.0f,std::min(1.0f,
        (transition.elapsedMs-transition.delayMs)/transition.durationMs));
    float low=0,high=1;
    for(int iteration=0;iteration<18;++iteration){
        const float middle=(low+high)/2;
        if(CubicCoordinate(middle,transition.x1,transition.x2)<linear)low=middle;else high=middle;
    }
    return CubicCoordinate((low+high)/2,transition.y1,transition.y2);
}

std::wstring TransitionValue(const StyleTransition& transition) {
    const float progress=TransitionProgress(transition);
    if(transition.discrete)return progress>=1?transition.to:transition.from;
    const auto from=NumericTokens(transition.from),to=NumericTokens(transition.to);
    if(from.empty()||from.size()!=to.size())return progress>=1?transition.to:transition.from;
    std::wstring result;size_t cursor=0;
    for(size_t index=0;index<to.size();++index){
        result.append(transition.to,cursor,to[index].offset-cursor);
        result+=NumberText(from[index].value+(to[index].value-from[index].value)*progress);
        cursor=to[index].offset+to[index].length;
    }
    result.append(transition.to,cursor,std::wstring::npos);return result;
}

bool TransitionComplete(const StyleTransition& transition) {
    return transition.elapsedMs>=transition.delayMs+transition.durationMs;
}

bool IsColorToken(const std::wstring& token) {
    const auto value=ToLower(Trim(token));
    return !value.empty()&&(value[0]==L'#'||value.rfind(L"rgb",0)==0||value.rfind(L"color-mix(",0)==0||value==L"transparent"||
        value==L"white"||value==L"black"||value==L"red"||value==L"blue"||
        value==L"green"||value==L"gray"||value==L"grey"||value==L"orange");
}

std::vector<BoxShadow> BoxShadows(const ComputedStyle& style,float viewport) {
    std::vector<BoxShadow> result;const auto raw=Trim(ToLower(style.Get(L"box-shadow")));
    if(raw.empty()||raw==L"none")return result;
    for(const auto& item:CommaSeparated(raw)){
        BoxShadow shadow;std::vector<std::wstring> lengths;bool valid=true;
        shadow.color=StyleSheet::Color(style.Get(L"color",L"#000000"),0xff000000);
        for(const auto& token:Words(item)){
            if(token==L"inset")shadow.inset=true;
            else if(IsColorToken(token))shadow.color=StyleSheet::Color(token,shadow.color);
            else lengths.push_back(token);
        }
        if(lengths.size()<2||lengths.size()>4)valid=false;
        if(valid){
            shadow.offsetX=StyleSheet::Length(lengths[0],0,viewport,0);
            shadow.offsetY=StyleSheet::Length(lengths[1],0,viewport,0);
            if(lengths.size()>2)shadow.blur=std::max(0.0f,StyleSheet::Length(lengths[2],0,viewport,0));
            if(lengths.size()>3)shadow.spread=StyleSheet::Length(lengths[3],0,viewport,0);
            result.push_back(shadow);
        }
    }
    return result;
}

Edges EdgeValues(const ComputedStyle& style,const std::wstring& base,float reference,float viewport) {
    static thread_local FastMap<EdgeCacheKey,CachedEdges,EdgeCacheHash> cache;
    const EdgeCacheKey key{style.values.get(),reference,viewport,base==L"padding"};
    auto found=cache.find(key);
    if(found!=cache.end()){
        const auto owner=found->second.owner.lock();
        if(owner&&owner.get()==style.values.get())return found->second.value;
        cache.clear();
    }else if(cache.size()>=8192)cache.clear();
    Edges e; auto values=Words(style.Get(base,L"0"));
    auto len=[&](const std::wstring& v){return StyleSheet::Length(v,reference,viewport,0,FontSize(style));};
    if(values.size()==1)e.top=e.right=e.bottom=e.left=len(values[0]);
    else if(values.size()==2){e.top=e.bottom=len(values[0]);e.left=e.right=len(values[1]);}
    else if(values.size()==3){e.top=len(values[0]);e.left=e.right=len(values[1]);e.bottom=len(values[2]);}
    else if(values.size()>=4){e.top=len(values[0]);e.right=len(values[1]);e.bottom=len(values[2]);e.left=len(values[3]);}
    for(auto [name,side]:std::vector<std::pair<std::wstring,float*>>{{base+L"-top",&e.top},{base+L"-right",&e.right},{base+L"-bottom",&e.bottom},{base+L"-left",&e.left}}){auto v=style.Get(name);if(!v.empty())*side=len(v);}
    cache.emplace(key,CachedEdges{style.values,e});
    return e;
}

float BorderWidth(const ComputedStyle& style,const std::wstring& side=L"") {
    auto borderStyle=side.empty()?style.Get(L"border-style"):
        style.Get(L"border-"+side+L"-style",style.Get(L"border-style"));
    if(borderStyle.empty())borderStyle=side.empty()?style.Get(L"border"):
        style.Get(L"border-"+side,style.Get(L"border"));
    std::wistringstream styleTokens(ToLower(borderStyle));std::wstring styleToken;
    while(styleTokens>>styleToken)if(styleToken==L"none"||styleToken==L"hidden")return 0;
    auto base=style.Get(L"border-width",style.Get(L"border"));
    auto value=side.empty()?base:style.Get(L"border-"+side+L"-width",style.Get(L"border-"+side,base));
    const auto snap=[&](float width){
        if(width<=0)return 0.0f;
        const auto scale=std::max(0.01f,style.deviceScale);
        return std::max(1.0f,std::floor(width*scale+0.0001f))/scale;
    };
    std::wistringstream widthTokens(ToLower(value));std::wstring widthToken;
    while(widthTokens>>widthToken){
        if(widthToken==L"thin")return snap(1.0f);if(widthToken==L"medium")return snap(3.0f);if(widthToken==L"thick")return snap(5.0f);
        try{size_t used=0;std::stof(widthToken,&used);if(used>0)return snap(StyleSheet::Length(widthToken,0,0,0));}catch(...){ }
    }
    return borderStyle.empty()?0.0f:snap(3.0f);
}

Edges BorderValues(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(cached.bordersValid&&cached.borderScale==style.deviceScale)return cached.borders;
    cached.borders.top=BorderWidth(style,L"top");cached.borders.right=BorderWidth(style,L"right");
    cached.borders.bottom=BorderWidth(style,L"bottom");cached.borders.left=BorderWidth(style,L"left");
    cached.borderScale=style.deviceScale;cached.bordersValid=true;return cached.borders;
}

float FontSize(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(!cached.fontSizeValid){
        cached.fontSize=StyleSheet::Length(style.Get(L"font-size",L"16px"),16,16,16);
        cached.fontSizeValid=true;
    }return cached.fontSize;
}
float LineHeight(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(cached.lineHeightValid)return cached.lineHeight;
    const auto raw=style.Get(L"line-height");const float font=FontSize(style);
    constexpr float normalLineHeight=4.0f/3.0f;
    if(raw.empty()||raw==L"normal")cached.lineHeight=font*normalLineHeight;
    else try{size_t used=0;const float multiple=std::stof(raw,&used);cached.lineHeight=used==raw.size()?font*multiple:StyleSheet::Length(raw,font,font,font*normalLineHeight);}
    catch(...){cached.lineHeight=StyleSheet::Length(raw,font,font,font*normalLineHeight);}
    cached.lineHeightValid=true;return cached.lineHeight;
}

IDWriteFactory* SharedWriteFactory();
Microsoft::WRL::ComPtr<IDWriteTextFormat> TextFormat(IDWriteFactory* factory,
                                                      const ComputedStyle& style);

float InlineFormattingDescent(const ComputedStyle& style){
    // An atomic inline-level box uses its bottom margin edge as its baseline.
    // The containing line therefore keeps the parent font's descent below the
    // box. Measure that descent in CSS DIPs; the render target applies the
    // monitor DPI later, so this remains correct at 100%, 150%, and per-monitor
    // DPI changes.
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        constexpr wchar_t sample[]=L" ";
        if(format&&SUCCEEDED(factory->CreateGdiCompatibleTextLayout(sample,1,
            format.Get(),100000.0f,100000.0f,1.0f,&identity,FALSE,&layout))){
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count)
                return std::max(0.0f,metrics.height-metrics.baseline);
        }
    }
    return FontSize(style)*0.25f;
}

bool IsAtomicInlineLevel(const LayoutBox& box){
    const auto display=box.style.Get(L"display");
    return display==L"inline-block"||display==L"inline-flex"||display==L"inline-grid";
}

float GapValue(const ComputedStyle& style,bool horizontal,float reference,float viewport) {
    const auto property=horizontal?L"column-gap":L"row-gap";
    return StyleSheet::Length(style.Get(property,style.Get(L"gap",L"0")),reference,viewport,0,FontSize(style));
}

std::wstring NormalizeText(const std::wstring& text,bool preserve,bool preserveLeading=false,bool preserveTrailing=false) {
    if(preserve)return text;std::wstring out;bool space=false;
    for(wchar_t c:text){if(std::iswspace(c)){if(!space&&(!out.empty()||preserveLeading))out+=L' ';space=true;}else{out+=c;space=false;}}
    if(!preserveTrailing&&!out.empty()&&out.back()==L' ')out.pop_back();
    return out;
}

bool PreservesSpaces(const std::wstring& whiteSpace){
    return whiteSpace==L"pre"||whiteSpace==L"pre-wrap"||whiteSpace==L"break-spaces";
}

bool PreservesLineBreaks(const std::wstring& whiteSpace){
    return PreservesSpaces(whiteSpace)||whiteSpace==L"pre-line";
}

bool PreventsTextWrapping(const std::wstring& whiteSpace){
    return whiteSpace==L"nowrap"||whiteSpace==L"pre";
}

std::wstring NormalizeText(const std::wstring& text,const std::wstring& whiteSpace,
                           bool preserveLeading=false,bool preserveTrailing=false) {
    if(PreservesSpaces(whiteSpace)){
        std::wstring out;out.reserve(text.size());
        for(size_t index=0;index<text.size();++index){
            if(text[index]==L'\r'){
                if(index+1<text.size()&&text[index+1]==L'\n')++index;
                out+=L'\n';
            }else out+=text[index];
        }
        return out;
    }
    if(whiteSpace!=L"pre-line")return NormalizeText(text,false,preserveLeading,preserveTrailing);
    std::wstring out;bool pendingSpace=false,lineHasText=false;
    for(size_t index=0;index<text.size();++index){
        wchar_t character=text[index];
        if(character==L'\r'||character==L'\n'){
            if(character==L'\r'&&index+1<text.size()&&text[index+1]==L'\n')++index;
            if(!out.empty()&&out.back()==L' ')out.pop_back();
            out+=L'\n';pendingSpace=false;lineHasText=false;continue;
        }
        if(std::iswspace(character)){pendingSpace=lineHasText;continue;}
        if(pendingSpace)out+=L' ';
        out+=character;pendingSpace=false;lineHasText=true;
    }
    return out;
}

std::uint64_t HashText(const std::wstring& value){std::uint64_t hash=1469598103934665603ull;for(wchar_t c:value){hash^=static_cast<std::uint64_t>(c);hash*=1099511628211ull;}return hash;}
void MixHash(std::uint64_t& seed,std::uint64_t value){seed^=value+0x9e3779b97f4a7c15ull+(seed<<6)+(seed>>2);}

std::uint64_t StyleContextHash(const std::shared_ptr<Node>& node,std::uint64_t parentHash,
                               const StyleSheet& styleSheet,size_t knownIndex=0,size_t knownCount=0,
                               const std::shared_ptr<Node>& knownPrevious={}){
    std::uint64_t hash=parentHash;MixHash(hash,HashText(node->tag));MixHash(hash,static_cast<std::uint64_t>(node->type));if(node->type==NodeType::Text)return hash;
    std::uint64_t attributes=0;for(const auto& item:node->attributes)if(styleSheet.AttributeAffectsStyle(item.first)){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));attributes^=pair;}MixHash(hash,attributes);
    std::uint64_t inlineStyle=0;for(const auto& item:node->inlineStyle){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));inlineStyle^=pair;}MixHash(hash,inlineStyle);
    MixHash(hash,(node->checked?1ull:0ull)|(node->disabled?2ull:0ull)|(node->hovered?4ull:0ull)|(node->focused?8ull:0ull)|(node->focusVisible?16ull:0ull)|(node->focusWithin?128ull:0ull));auto parent=node->parent.lock();if(parent){bool first=false,last=false;std::uint64_t childIndex=0;std::shared_ptr<Node> previous;if(knownIndex){childIndex=knownIndex;first=knownIndex==1;last=knownIndex==knownCount;previous=knownPrevious;}else{std::uint64_t currentIndex=0;for(const auto& sibling:parent->children)if(sibling->type==NodeType::Element){++currentIndex;if(sibling==node){first=currentIndex==1;childIndex=currentIndex;break;}previous=sibling;}for(auto it=parent->children.rbegin();it!=parent->children.rend();++it)if((*it)->type==NodeType::Element){last=*it==node;break;}}MixHash(hash,(first?32ull:0ull)|(last?64ull:0ull));if(styleSheet.UsesNthChildFor(node))MixHash(hash,childIndex);if(previous){MixHash(hash,HashText(previous->tag));MixHash(hash,(previous->checked?1ull:0ull)|(previous->disabled?2ull:0ull)|(previous->focused?4ull:0ull)|(previous->focusVisible?8ull:0ull));std::uint64_t siblingAttributes=0;for(const auto& item:previous->attributes)if(styleSheet.AttributeAffectsStyle(item.first)){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));siblingAttributes^=pair;}MixHash(hash,siblingAttributes);}}return hash;
}

std::wstring FontFamily(const ComputedStyle& style) {
    auto family=style.Get(L"font-family",L"Malgun Gothic");
    const auto comma=family.find(L',');if(comma!=std::wstring::npos)family=Trim(family.substr(0,comma));
    family.erase(std::remove(family.begin(),family.end(),L'\''),family.end());
    family.erase(std::remove(family.begin(),family.end(),L'"'),family.end());
    return family;
}

bool IsHangul(wchar_t c) {
    return (c>=0x1100&&c<=0x11ff)||(c>=0x3130&&c<=0x318f)||
           (c>=0xac00&&c<=0xd7af);
}

bool ContainsHangul(const std::wstring& text) {
    return std::any_of(text.begin(),text.end(),IsHangul);
}

bool UsesAutomaticHangulFallback(const ComputedStyle& style) {
    const auto family=ToLower(FontFamily(style));
    return family!=L"arial"&&family.find(L"noto sans kr")==std::wstring::npos&&
           family.find(L"malgun gothic")==std::wstring::npos&&
           family.find(L"yu gothic")==std::wstring::npos;
}

bool IsFormControlText(const LayoutBox& box) {
    if(!box.node)return false;
    if(box.node->tag==L"input"||box.node->tag==L"select"||box.node->tag==L"textarea")return true;
    // An anonymous text run directly owned by a button participates in the
    // native control's metrics. Descendant elements create ordinary CSS boxes
    // and retain standard metrics even when their font is inherited.
    return box.parent&&box.parent->node->tag==L"button";
}

bool IsButtonControlText(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock())
        if(current->tag==L"button")return true;
    return false;
}

bool IsDecorativeControlText(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock()){
        if(ToLower(Trim(current->Attribute(L"aria-hidden")))==L"true")return true;
        if(current->tag==L"button")break;
    }
    return false;
}

int FontWeight(const ComputedStyle& style) {
    try{return std::max(1,std::min(999,std::stoi(style.Get(L"font-weight",L"400"))));}
    catch(...){return style.Is(L"font-weight",L"bold")?700:400;}
}

float BrowserSymbolAdvanceAdjustment(wchar_t value,float size) {
    // DirectWrite and Chromium choose different fallback advances for a few
    // common Windows UI symbols. Express the difference in em units so the
    // inline geometry remains stable across font sizes and monitor DPI.
    switch(value){
        case 0x2191:return size*0.065f; // upwards arrow
        case 0x2193:return size*0.05f; // downwards arrow
        case 0x21bb:return size*0.04f; // clockwise open circle arrow
        case 0x2442:
        case 0x2443:return size*-0.025f; // OCR branch/merge marks
        case 0x25c7:return size*0.20f; // white diamond
        case 0x25cf:return size*-0.37f; // black circle
        default:return 0;
    }
}

DWRITE_FONT_STYLE FontStyle(const ComputedStyle& style) {
    const auto value=ToLower(Trim(style.Get(L"font-style",L"normal")));
    return value==L"italic"?DWRITE_FONT_STYLE_ITALIC:
        (value==L"oblique"?DWRITE_FONT_STYLE_OBLIQUE:DWRITE_FONT_STYLE_NORMAL);
}

IDWriteFactory* SharedWriteFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> factory=[] {
        Microsoft::WRL::ComPtr<IDWriteFactory> value;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(value.ReleaseAndGetAddressOf()));
        return value;
    }();
    return factory.Get();
}

Microsoft::WRL::ComPtr<IDWriteTextFormat> TextFormat(IDWriteFactory* factory,
                                                      const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,Microsoft::WRL::ComPtr<IDWriteTextFormat>> cache;
    std::wstring key=std::to_wstring(reinterpret_cast<std::uintptr_t>(factory));key+=L'\x1f';
    key+=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));key+=L'\x1f';
    key+=std::to_wstring(FontWeight(style));key+=L'\x1f';key+=style.Get(L"font-style");
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if(factory)factory->CreateTextFormat(FontFamily(style).c_str(),nullptr,
        static_cast<DWRITE_FONT_WEIGHT>(FontWeight(style)),FontStyle(style),
        DWRITE_FONT_STRETCH_NORMAL,FontSize(style),L"ko-kr",&format);
    if(format)cache.emplace(std::move(key),format);
    return format;
}

float TextBaselineOffset(const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,float> cache;
    std::wstring key=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));
    key+=L'\x1f';key+=std::to_wstring(FontWeight(style));key+=L'\x1f';
    key+=style.Get(L"font-style");key+=L'\x1f';key+=style.Get(L"line-height");
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    float result=LineHeight(style)*0.8f;
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        constexpr wchar_t sample[]=L"Hg";
        if(format&&SUCCEEDED(factory->CreateTextLayout(sample,2,format.Get(),
            100000.0f,100000.0f,&layout))){
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count)
                result=std::max(0.0f,(LineHeight(style)-metrics.height)/2.0f+
                    metrics.baseline);
        }
    }
    cache.emplace(std::move(key),result);return result;
}

const ComputedStyle* FirstFlexBaselineStyle(const LayoutBox& box) {
    if(box.node&&box.node->type==NodeType::Text)return &box.style;
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||
           child->style.Is(L"position",L"fixed"))continue;
        if(const auto* style=FirstFlexBaselineStyle(*child))return style;
    }
    return nullptr;
}

float FlexItemBaselineOffset(const LayoutBox& box,float referenceWidth,
                             float outerHeight) {
    const auto margin=EdgeValues(box.style,L"margin",referenceWidth,referenceWidth);
    const auto padding=EdgeValues(box.style,L"padding",referenceWidth,referenceWidth);
    const auto border=BorderValues(box.style);
    if(const auto* textStyle=FirstFlexBaselineStyle(box))
        return margin.top+border.top+padding.top+TextBaselineOffset(*textStyle);
    // CSS synthesizes a baseline at the item's bottom margin edge when the
    // item has no usable first baseline.
    return std::max(margin.top,outerHeight-margin.bottom);
}

void ApplyFontFallback(IDWriteFactory* factory,IDWriteTextLayout* layout,
                       const std::wstring& text,const ComputedStyle& style,
                       bool controlMetrics=false) {
    if(!factory||!layout)return;
    const auto primaryFamily=ToLower(FontFamily(style));
    const bool primaryProvidesKorean=primaryFamily.find(L"noto sans kr")!=std::wstring::npos||
        primaryFamily.find(L"malgun gothic")!=std::wstring::npos||
        primaryFamily.find(L"yu gothic")!=std::wstring::npos;
    if(!controlMetrics&&!primaryProvidesKorean&&ContainsHangul(text)){
        // A CSS family such as Segoe UI or Arial does not contain Hangul.
        // Chromium resolves the Korean runs through the installed UI fallback
        // face while DirectWrite's implicit fallback can choose a wider legacy
        // face. Pin only those missing-glyph runs to the available Korean UI
        // font so layout measurement and painting use the browser fallback.
        static const bool hasKoreanUiFont=[factory] {
            Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
            UINT32 index=0;BOOL exists=FALSE;
            return SUCCEEDED(factory->GetSystemFontCollection(&collection))&&
                   SUCCEEDED(collection->FindFamilyName(L"Noto Sans KR",&index,&exists))&&exists;
        }();
        if(hasKoreanUiFont)for(size_t start=0;start<text.size();){
            if(!IsHangul(text[start])){++start;continue;}
            size_t end=start+1;
            while(end<text.size()&&IsHangul(text[end]))++end;
            layout->SetFontFamilyName(L"Noto Sans KR",
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
    if(controlMetrics){
        // Browser form controls resolve arrows, geometric shapes and technical
        // symbols through the Windows symbol face instead of borrowing the
        // surrounding UI face's wider notdef/fallback advance.
        for(size_t start=0;start<text.size();){
            const auto code=static_cast<unsigned int>(text[start]);
            const bool symbol=(code>=0x2190&&code<=0x27ff)&&code!=0x25cf;
            if(!symbol){++start;continue;}
            size_t end=start+1;
            while(end<text.size()){
                const auto next=static_cast<unsigned int>(text[end]);
                if(next<0x2190||next>0x27ff)break;
                ++end;
            }
            layout->SetFontFamilyName(L"Segoe UI Symbol",
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
}

void ApplyCharacterSpacing(IDWriteTextLayout* layout,const std::wstring& text,
                           const ComputedStyle& style,bool controlMetrics=false) {
    if(!layout||text.empty())return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout1> extended;
    if(FAILED(layout->QueryInterface(IID_PPV_ARGS(&extended))))return;

    float authorSpacing=0;
    const auto rawSpacing=style.Get(L"letter-spacing");
    if(!rawSpacing.empty()&&rawSpacing!=L"normal")
        authorSpacing=StyleSheet::Length(rawSpacing,FontSize(style),FontSize(style),0,FontSize(style));
    if(std::abs(authorSpacing)>0.001f)
        extended->SetCharacterSpacing(0,authorSpacing,0,
            DWRITE_TEXT_RANGE{0,static_cast<UINT32>(text.size())});

    // Chromium uses GDI-compatible metrics for native form-control text on
    // Windows. Keep those small fallback/space adjustments out of ordinary
    // document text so large headings retain their authored letter spacing.
    const float size=FontSize(style);
    if(controlMetrics&&UsesAutomaticHangulFallback(style)){
        const float hangulSpacing=authorSpacing-size*0.08f;
        for(size_t start=0;start<text.size();){
            if(!IsHangul(text[start])){++start;continue;}
            size_t end=start+1;
            while(end<text.size()&&IsHangul(text[end]))++end;
            extended->SetCharacterSpacing(0,hangulSpacing,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
    if(controlMetrics&&size>11.0f)for(size_t index=0;index<text.size();++index)
            if(text[index]==L' ')
                extended->SetCharacterSpacing(0,authorSpacing-(size-11.0f)*0.216f,0,
                    DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});

    for(size_t index=0;index<text.size();++index){
        const float adjustment=BrowserSymbolAdvanceAdjustment(text[index],size);
        if(std::abs(adjustment)>0.001f)
            extended->SetCharacterSpacing(0,authorSpacing+adjustment,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});
    }

    for(size_t index=0;index<text.size();++index)
        if(text[index]==0x21f2)
            extended->SetCharacterSpacing(0,authorSpacing-1.0f,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});
}

float SpaceAdvance(IDWriteFactory* factory,IDWriteTextFormat* format,
                   const ComputedStyle& style,bool controlMetrics) {
    if(!factory||!format)return FontSize(style)*0.5f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    constexpr wchar_t space[]=L" ";HRESULT created=E_FAIL;
    if(controlMetrics){
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        created=factory->CreateGdiCompatibleTextLayout(space,1,format,100.0f,100.0f,
            1.0f,&identity,FALSE,&layout);
    }else created=factory->CreateTextLayout(space,1,format,100.0f,100.0f,&layout);
    if(FAILED(created))return FontSize(style)*0.5f;
    ApplyFontFallback(factory,layout.Get(),space,style,controlMetrics);
    ApplyCharacterSpacing(layout.Get(),space,style,controlMetrics);
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics))?
        std::max(0.01f,metrics.widthIncludingTrailingWhitespace):FontSize(style)*0.5f;
}

void ApplyTabSize(IDWriteFactory* factory,IDWriteTextLayout* layout,
                  IDWriteTextFormat* format,const std::wstring& text,
                  const ComputedStyle& style,bool controlMetrics=false) {
    if(!layout||text.find(L'\t')==std::wstring::npos)return;
    const auto raw=ToLower(Trim(style.Get(L"tab-size",L"8")));
    float stop=0;bool number=false;
    try{
        size_t used=0;const float value=std::stof(raw,&used);
        if(used==raw.size()){
            stop=SpaceAdvance(factory,format,style,controlMetrics)*value;
            number=true;
        }
    }catch(...){}
    if(!number)stop=StyleSheet::Length(raw,FontSize(style),FontSize(style),
        SpaceAdvance(factory,format,style,controlMetrics)*8.0f,FontSize(style));
    layout->SetIncrementalTabStop(std::max(0.01f,stop));
}

void ApplyTextDecorations(IDWriteTextLayout* layout,const std::wstring& text,
                          const ComputedStyle& style) {
    if(!layout||text.empty())return;
    const auto decoration=ToLower(style.Get(L"text-decoration-line",
        style.Get(L"text-decoration",L"none")));
    const DWRITE_TEXT_RANGE range{0,static_cast<UINT32>(text.size())};
    if(decoration.find(L"underline")!=std::wstring::npos)layout->SetUnderline(TRUE,range);
    if(decoration.find(L"line-through")!=std::wstring::npos)layout->SetStrikethrough(TRUE,range);
}

float TextWidth(const std::wstring& source,const ComputedStyle& style,bool preserveLeading=false,bool preserveTrailing=false,bool gdiCompatible=false){
    const auto text=NormalizeText(source,style.Get(L"white-space"),preserveLeading,preserveTrailing);
    static thread_local FastMap<std::wstring,float> cache;
    std::wstring cacheKey=text;cacheKey+=L'\x1f';cacheKey+=FontFamily(style);cacheKey+=L'\x1f';
    cacheKey+=NumberText(FontSize(style));cacheKey+=L'\x1f';cacheKey+=std::to_wstring(FontWeight(style));
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-style");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"letter-spacing");cacheKey+=L'\x1f';cacheKey+=style.Get(L"tab-size");
    cacheKey+=gdiCompatible?L"\x1fG":L"\x1fD";
    if(const auto found=cache.find(cacheKey);found!=cache.end())return found->second;
    if(cache.size()>8192)cache.clear();
    auto remember=[&](float value){cache[std::move(cacheKey)]=value;return value;};
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        HRESULT created=E_FAIL;
        if(format){
            if(gdiCompatible){
                const DWRITE_MATRIX identity{1,0,0,1,0,0};
                created=factory->CreateGdiCompatibleTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                    format.Get(),100000.0f,100000.0f,1.0f,&identity,FALSE,&layout);
            }else created=factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                format.Get(),100000.0f,100000.0f,&layout);
        }
        if(SUCCEEDED(created)){
            ApplyFontFallback(factory,layout.Get(),text,style,gdiCompatible);
            ApplyTabSize(factory,layout.Get(),format.Get(),text,style,gdiCompatible);
            DWRITE_TEXT_METRICS metrics{};
            if(SUCCEEDED(layout->GetMetrics(&metrics))){
                float width=metrics.widthIncludingTrailingWhitespace;
                const auto rawSpacing=style.Get(L"letter-spacing");
                if(!rawSpacing.empty()&&rawSpacing!=L"normal")
                    width+=StyleSheet::Length(rawSpacing,FontSize(style),FontSize(style),0,FontSize(style))*text.size();
                const float size=FontSize(style);
                if(gdiCompatible&&UsesAutomaticHangulFallback(style))
                    width-=size*0.08f*std::count_if(text.begin(),text.end(),IsHangul);
                if(gdiCompatible){
                    if(size>11.0f)
                        width-=(size-11.0f)*0.216f*std::count(text.begin(),text.end(),L' ');
                }
                for(const auto character:text)
                    width+=BrowserSymbolAdvanceAdjustment(character,size);
                width-=static_cast<float>(std::count(text.begin(),text.end(),static_cast<wchar_t>(0x21f2)));
                // Chromium stores inline geometry in 1/64 CSS-pixel layout
                // units. Round the measured advance upward to that boundary so
                // DirectWrite cannot wrap the final glyph because of a smaller
                // floating-point width at paint time.
                return remember(std::max(1.0f,std::ceil(width*64.0f)/64.0f));
            }
        }
    }
    const float fontSize=FontSize(style);float em=0;
    for(wchar_t c:text){
        if(std::iswspace(c))em+=0.34f;
        else if((c>=0x2e80&&c<=0xd7af)||(c>=0xf900&&c<=0xfaff))em+=1.0f;
        else if(std::iswupper(c))em+=0.66f;
        else if(std::iswdigit(c))em+=0.58f;
        else if(std::iswpunct(c))em+=0.42f;
        else em+=0.57f;
    }
    return remember(std::max(1.0f,em*fontSize+1.0f));
}

float TextHeight(const std::wstring& source,const ComputedStyle& style,float availableWidth,
                 bool preserveLeading=false,bool preserveTrailing=false){
    const auto whiteSpace=style.Get(L"white-space");
    // Collapsed no-wrap content always occupies one line. Preformatted modes
    // still need measurement because authored segment breaks create lines.
    if(whiteSpace==L"nowrap")return LineHeight(style);
    const auto text=NormalizeText(source,whiteSpace,preserveLeading,preserveTrailing);
    static thread_local FastMap<std::wstring,float> cache;
    std::wstring cacheKey=text;cacheKey+=L'\x1f';cacheKey+=FontFamily(style);cacheKey+=L'\x1f';
    cacheKey+=NumberText(FontSize(style));cacheKey+=L'\x1f';cacheKey+=std::to_wstring(FontWeight(style));
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-style");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"letter-spacing");cacheKey+=L'\x1f';cacheKey+=style.Get(L"tab-size");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"line-height");cacheKey+=L'\x1f';cacheKey+=whiteSpace;cacheKey+=L'\x1f';
    cacheKey+=NumberText(std::round(availableWidth*64.0f)/64.0f);
    if(const auto found=cache.find(cacheKey);found!=cache.end())return found->second;
    if(cache.size()>8192)cache.clear();
    auto remember=[&](float value){cache[std::move(cacheKey)]=value;return value;};
    if((!PreservesLineBreaks(whiteSpace)||text.find(L'\n')==std::wstring::npos)&&
       TextWidth(text,style,preserveLeading,preserveTrailing)<=availableWidth+0.5f)
        return remember(LineHeight(style));
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(format&&SUCCEEDED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
            format.Get(),std::max(1.0f,availableWidth),100000.0f,&layout))){
            if(PreventsTextWrapping(whiteSpace))layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ApplyFontFallback(factory,layout.Get(),text,style);
            ApplyCharacterSpacing(layout.Get(),text,style);
            ApplyTabSize(factory,layout.Get(),format.Get(),text,style);
            UINT32 lineCount=0;layout->GetLineMetrics(nullptr,0,&lineCount);
            if(lineCount)return remember(LineHeight(style)*lineCount);
        }
    }
    return remember(LineHeight(style)*std::max(1.0f,std::ceil(TextWidth(text,style)/std::max(1.0f,availableWidth))));
}

float ControlLineHeight(const std::wstring& source,const ComputedStyle& style) {
    const auto raw=style.Get(L"line-height");
    if(!raw.empty()&&raw!=L"normal")return LineHeight(style);
    if(auto* factory=SharedWriteFactory()){
        const auto text=source.empty()?L" ":source;
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        if(format&&SUCCEEDED(factory->CreateGdiCompatibleTextLayout(text.c_str(),
            static_cast<UINT32>(text.size()),format.Get(),100000.0f,100000.0f,
            1.0f,&identity,FALSE,&layout))){
            ApplyFontFallback(factory,layout.Get(),text,style,true);
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count)
                return std::max(LineHeight(style),std::ceil(metrics.height));
        }
    }
    return LineHeight(style);
}

std::wstring SelectedOptionText(const std::shared_ptr<Node>& select) {
    if(!select||select->tag!=L"select")return L"";
    const auto value=select->Attribute(L"value");
    std::vector<std::shared_ptr<Node>> options;
    std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
        for(const auto& child:parent->children){
            if(child->tag==L"option")options.push_back(child);
            else if(child->tag==L"optgroup")collect(child);
        }
    };
    collect(select);
    std::shared_ptr<Node> fallback;
    for(const auto& option:options){
        if(!fallback||option->attributes.count(L"selected"))fallback=option;
        const auto optionValue=option->attributes.count(L"value")?option->Attribute(L"value"):option->InnerText();
        if(select->attributes.count(L"value")&&optionValue==value)return option->InnerText();
    }
    return fallback?fallback->InnerText():L"";
}

std::wstring BoxText(const LayoutBox& box,bool* placeholder=nullptr) {
    if(placeholder)*placeholder=false;
    if(box.node->type==NodeType::Text)
        return NormalizeText(box.node->text,box.style.Get(L"white-space"),
                             box.preserveLeadingWhitespace,box.preserveTrailingWhitespace);
    if((box.node->tag==L"input"&&box.node->Attribute(L"type")!=L"checkbox"&&
        box.node->Attribute(L"type")!=L"radio")||box.node->tag==L"textarea"){
        auto text=box.node->Attribute(L"value");
        if(box.node->tag==L"input"&&ToLower(box.node->Attribute(L"type"))==L"password"&&
           !text.empty())text.assign(text.size(),L'\x2022');
        if(text.empty()){
            text=box.node->Attribute(L"placeholder");
            if(placeholder)*placeholder=!text.empty();
        }
        return text;
    }
    return box.node->tag==L"select"?SelectedOptionText(box.node):std::wstring{};
}

D2D1_POINT_2F TextOrigin(const LayoutBox& box) {
    const float selectInset=box.node->tag==L"select"?4.0f:0.0f;
    const float selectTop=box.node->tag==L"select"?1.0f:0.0f;
    return D2D1::Point2F(box.content.x+selectInset,box.content.y+selectTop-
        (box.node->tag==L"textarea"?box.node->scrollTop:0.0f));
}

void EnsureTextLayout(LayoutBox& box,IDWriteFactory* factory,const std::wstring& text) {
    if(text.empty()||!factory){box.textLayout.Reset();box.textLayoutKey.clear();return;}
    const bool formControl=IsFormControlText(box);
    const auto whiteSpace=box.style.Get(L"white-space");
    const float nativeSelectInset=box.node->tag==L"select"?4.0f:0.0f;
    std::wstring layoutKey=text;layoutKey+=L'\x1f';layoutKey+=FontFamily(box.style);layoutKey+=L'\x1f';
    layoutKey+=NumberText(FontSize(box.style));layoutKey+=L'\x1f';layoutKey+=std::to_wstring(FontWeight(box.style));
    layoutKey+=L'\x1f';layoutKey+=box.style.Get(L"font-style");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"letter-spacing");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"tab-size");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"text-decoration-line",box.style.Get(L"text-decoration"));layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"text-align");layoutKey+=L'\x1f';layoutKey+=whiteSpace;layoutKey+=L'\x1f';
    const float textLayoutHeight=box.node->tag==L"textarea"?
        std::max(box.content.height,box.scrollHeight):box.content.height;
    layoutKey+=NumberText(box.content.width);layoutKey+=L',';layoutKey+=NumberText(textLayoutHeight);
    layoutKey+=formControl?L"\x1fG":L"\x1fD";
    if(box.textLayout&&box.textLayoutKey==layoutKey)return;
    box.textLayout.Reset();auto format=TextFormat(factory,box.style);if(!format)return;
    const DWRITE_MATRIX identity{1,0,0,1,0,0};
    const auto created=formControl?
        factory->CreateGdiCompatibleTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
            format.Get(),std::max(1.0f,box.content.width-nativeSelectInset),
            std::max(1.0f,textLayoutHeight),1.0f,&identity,FALSE,&box.textLayout):
        factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),
            std::max(1.0f,box.content.width),std::max(1.0f,textLayoutHeight),&box.textLayout);
    if(FAILED(created)){box.textLayout.Reset();return;}
    if(box.style.Is(L"text-align",L"center"))box.textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else if(box.style.Is(L"text-align",L"right"))box.textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    box.textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
        LineHeight(box.style),TextBaselineOffset(box.style));
    box.textLayout->SetParagraphAlignment(box.node->tag==L"textarea"?
        DWRITE_PARAGRAPH_ALIGNMENT_NEAR:DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if(box.node->tag==L"input"||box.node->tag==L"select"||PreventsTextWrapping(whiteSpace))
        box.textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ApplyFontFallback(factory,box.textLayout.Get(),text,box.style,formControl);
    ApplyCharacterSpacing(box.textLayout.Get(),text,box.style,formControl);
    ApplyTabSize(factory,box.textLayout.Get(),format.Get(),text,box.style,formControl);
    ApplyTextDecorations(box.textLayout.Get(),text,box.style);
    box.textLayoutKey=std::move(layoutKey);
}

float Constrain(const ComputedStyle& style,const wchar_t* minimum,const wchar_t* maximum,float value,float reference,float viewport){
    const auto minValue=style.Get(minimum);if(!minValue.empty()&&minValue!=L"auto")value=std::max(value,StyleSheet::Length(minValue,reference,viewport,value));
    const auto maxValue=style.Get(maximum);if(!maxValue.empty()&&maxValue!=L"none"&&maxValue!=L"auto")value=std::min(value,StyleSheet::Length(maxValue,reference,viewport,value));
    return value;
}

float ConstrainIntrinsicHeight(const ComputedStyle& style,float value,float viewport){
    const auto minimum=style.Get(L"min-height");
    if(!minimum.empty()&&minimum!=L"auto"&&minimum.find(L'%')==std::wstring::npos)
        value=std::max(value,StyleSheet::Length(minimum,500,viewport,value));
    const auto maximum=style.Get(L"max-height");
    if(!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"&&
       maximum.find(L'%')==std::wstring::npos)
        value=std::min(value,StyleSheet::Length(maximum,500,viewport,value));
    return value;
}

float NaturalWidth(const LayoutBox& box);

float BlockOuterWidth(const LayoutBox& box,float availableWidth,float viewportWidth){
    const auto margin=EdgeValues(box.style,L"margin",availableWidth,viewportWidth);
    const auto raw=box.style.Get(L"width");
    if(ToLower(Trim(raw))==L"max-content")return NaturalWidth(box);
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");const auto padding=EdgeValues(box.style,L"padding",availableWidth,viewportWidth);const auto border=BorderValues(box.style);
    const float decoration=borderBox?0.0f:padding.left+padding.right+border.left+border.right;const bool automatic=raw.empty()||raw==L"auto";
    float width=automatic?std::max(0.0f,availableWidth-margin.left-margin.right-decoration):StyleSheet::Length(raw,availableWidth,viewportWidth,std::max(0.0f,availableWidth-margin.left-margin.right-decoration));
    width=Constrain(box.style,L"min-width",L"max-width",width,availableWidth,viewportWidth)+decoration;
    return std::max(0.0f,width)+margin.left+margin.right;
}

float BlockOuterHeight(const LayoutBox& box,float availableHeight,float availableWidth,
                       float viewportHeight,float viewportWidth){
    const auto margin=EdgeValues(box.style,L"margin",availableWidth,viewportWidth);
    const auto raw=box.style.Get(L"height");
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");
    const auto padding=EdgeValues(box.style,L"padding",availableWidth,viewportWidth);
    const auto border=BorderValues(box.style);
    const float decoration=borderBox?0.0f:padding.top+padding.bottom+border.top+border.bottom;
    float height=StyleSheet::Length(raw,availableHeight,viewportHeight,
        std::max(0.0f,availableHeight-margin.top-margin.bottom-decoration));
    height=Constrain(box.style,L"min-height",L"max-height",height,availableHeight,viewportHeight)+decoration;
    return std::max(0.0f,height)+margin.top+margin.bottom;
}

size_t GridColumnCount(const LayoutBox& box,float availableWidth){
    const auto definition=box.style.Get(L"grid-template-columns",L"1fr");
    if(definition.find(L"repeat(auto-fit")!=std::wstring::npos||definition.find(L"repeat(auto-fill")!=std::wstring::npos){
        float minimum=320;const auto minmax=definition.find(L"minmax(");
        if(minmax!=std::wstring::npos){const auto comma=definition.find(L',',minmax);if(comma!=std::wstring::npos)minimum=StyleSheet::Length(definition.substr(minmax+7,comma-minmax-7),availableWidth,availableWidth,minimum);}
        const float gap=GapValue(box.style,true,availableWidth,availableWidth);
        const size_t fits=static_cast<size_t>(std::max(1.0f,std::floor((availableWidth+gap)/std::max(1.0f,minimum+gap))));
        return std::max<size_t>(1,std::min(fits,std::max<size_t>(1,box.children.size())));
    }
    const auto repeat=definition.find(L"repeat(");
    if(repeat!=std::wstring::npos){try{return std::max<size_t>(1,static_cast<size_t>(std::stoul(definition.substr(repeat+7))));}catch(...){} }
    return std::max<size_t>(1,Words(definition).size());
}

bool HasTableDisplay(const LayoutBox& box,const wchar_t* display){
    return box.visible&&box.style.Is(L"display",display);
}

bool IsTableRowGroup(const LayoutBox& box){
    const auto display=box.style.Get(L"display");
    return box.visible&&(display==L"table-header-group"||display==L"table-row-group"||
        display==L"table-footer-group");
}

size_t TableSpan(const std::shared_ptr<Node>& node,const wchar_t* attribute,
                 size_t maximum,bool zeroIsSpecial=false){
    const auto raw=Trim(node?node->Attribute(attribute):L"");
    if(raw.empty())return 1;
    size_t value=0;
    for(const wchar_t c:raw){
        if(c<L'0'||c>L'9')return 1;
        const size_t digit=static_cast<size_t>(c-L'0');
        if(value>(maximum-digit)/10)return maximum;
        value=value*10+digit;
    }
    if(value==0)return zeroIsSpecial?0:1;
    return std::min(value,maximum);
}

struct TableRowEntry {
    LayoutBox* box = nullptr;
    LayoutBox* group = nullptr;
};

struct TableCellEntry {
    LayoutBox* box = nullptr;
    size_t row = 0;
    size_t column = 0;
    size_t rowSpan = 1;
    size_t columnSpan = 1;
};

struct TableGridModel {
    std::vector<TableRowEntry> rows;
    std::vector<TableCellEntry> cells;
    size_t columnCount = 0;
};

TableGridModel BuildTableGrid(LayoutBox& table){
    TableGridModel model;
    std::function<void(LayoutBox&,LayoutBox*)> collectRows=
        [&](LayoutBox& current,LayoutBox* group){
            if(!current.visible)return;
            if(&current!=&table&&HasTableDisplay(current,L"table"))return;
            if(IsTableRowGroup(current))group=&current;
            if(HasTableDisplay(current,L"table-row")){
                model.rows.push_back({&current,group?group:&table});
                return;
            }
            for(auto& child:current.children)collectRows(*child,group);
        };
    for(auto& child:table.children)collectRows(*child,nullptr);
    if(model.rows.empty())return model;

    std::vector<size_t> groupEnds(model.rows.size());
    for(size_t begin=0;begin<model.rows.size();){
        size_t end=begin+1;
        while(end<model.rows.size()&&model.rows[end].group==model.rows[begin].group)++end;
        for(size_t row=begin;row<end;++row)groupEnds[row]=end;
        begin=end;
    }

    std::vector<std::vector<unsigned char>> occupied(model.rows.size());
    for(size_t row=0;row<model.rows.size();++row){
        size_t searchColumn=0;
        for(auto& child:model.rows[row].box->children){
            if(!HasTableDisplay(*child,L"table-cell")||
               child->style.Is(L"position",L"absolute")||
               child->style.Is(L"position",L"fixed"))continue;
            const size_t columnSpan=TableSpan(child->node,L"colspan",1000);
            const size_t authoredRowSpan=TableSpan(child->node,L"rowspan",65534,true);
            const size_t rowSpan=authoredRowSpan==0?groupEnds[row]-row:
                std::min(authoredRowSpan,groupEnds[row]-row);
            for(;;++searchColumn){
                if(occupied[row].size()<searchColumn+columnSpan)
                    occupied[row].resize(searchColumn+columnSpan);
                bool available=true;
                for(size_t column=searchColumn;column<searchColumn+columnSpan;++column)
                    if(occupied[row][column]){available=false;break;}
                if(available)break;
            }
            for(size_t targetRow=row;targetRow<row+rowSpan;++targetRow){
                if(occupied[targetRow].size()<searchColumn+columnSpan)
                    occupied[targetRow].resize(searchColumn+columnSpan);
                for(size_t column=searchColumn;column<searchColumn+columnSpan;++column)
                    occupied[targetRow][column]=1;
            }
            model.cells.push_back({child.get(),row,searchColumn,rowSpan,columnSpan});
            model.columnCount=std::max(model.columnCount,searchColumn+columnSpan);
            searchColumn+=columnSpan;
        }
        model.columnCount=std::max(model.columnCount,occupied[row].size());
    }
    return model;
}

std::vector<float> ResolveTableColumns(const LayoutBox& table,const TableGridModel& model,
                                       float availableWidth,float viewportWidth);
std::vector<float> ResolveTableRows(const TableGridModel& model,
                                    const std::vector<float>& columns);

float NaturalWidth(const LayoutBox& box){
    if(box.naturalWidthValid)return box.naturalWidth;
    auto remember=[&](float value){box.naturalWidth=value;box.naturalWidthValid=true;return value;};
    if(box.node->type==NodeType::Text){
        const float advance=TextWidth(box.node->text,box.style,box.preserveLeadingWhitespace,
                                      box.preserveTrailingWhitespace,IsFormControlText(box));
        // Inline siblings need enough width for the final painted glyph. The
        // compressed Hangul advance alone can leave ink over the next span.
        if(box.parent&&box.parent->style.Is(L"display",L"inline")&&
           box.parent->parent&&box.parent->parent->style.Is(L"display",L"inline")){
            const auto text=NormalizeText(box.node->text,false,box.preserveLeadingWhitespace,
                                          box.preserveTrailingWhitespace);
            return remember(advance+FontSize(box.style)*0.08f*
                std::count_if(text.begin(),text.end(),IsHangul));
        }
        return remember(advance);
    }
    if(box.node->tag==L"br")return remember(0.0f);
    auto width=box.style.Get(L"width");const auto normalizedWidth=ToLower(Trim(width));float value=0;
    // Percentages depend on the containing block and are indefinite during
    // intrinsic sizing.  Use the element's intrinsic contribution here; the
    // percentage is resolved later when the containing block is known.
    if(!width.empty()&&width!=L"auto"&&normalizedWidth!=L"max-content"&&
       width.find(L'%')==std::wstring::npos)
        value=StyleSheet::Length(width,500,500,0);
    else if(box.node->tag==L"input"&&(box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio"))value=13;
    else if(box.node->tag==L"input")value=160;
    else if(box.node->tag==L"select"){
        float optionWidth=TextWidth(SelectedOptionText(box.node),box.style);
        std::function<void(const std::shared_ptr<Node>&)> measureOptions=
            [&](const std::shared_ptr<Node>& node){
                for(const auto& child:node->children){
                    if(child->tag==L"option")
                        optionWidth=std::max(optionWidth,TextWidth(child->InnerText(),box.style));
                    else measureOptions(child);
                }
            };
        measureOptions(box.node);
        // Native select appearance reserves its leading text inset and the
        // disclosure-button area in addition to the widest option.
        value=optionWidth+20.0f;
    }
    else if(box.node->tag==L"svg"){
        const auto viewBox=SvgNumbers(box.node->Attribute(L"viewbox"));
        const auto height=box.style.Get(L"height",box.node->Attribute(L"height"));
        if(!height.empty()&&height!=L"auto"&&viewBox.size()==4&&viewBox[3]>0)
            value=StyleSheet::Length(height,500,500,150)*viewBox[2]/viewBox[3];
        else value=StyleSheet::Length(box.node->Attribute(L"width"),500,500,300);
    }
    else if(box.node->tag==L"button"&&box.style.Get(L"display")!=L"grid"&&
            box.style.Get(L"display")!=L"inline-grid"){
        auto padding=EdgeValues(box.style,L"padding",500,500);auto border=BorderValues(box.style);
        const auto display=box.style.Get(L"display");
        const bool flex=display==L"flex"||display==L"inline-flex";
        const bool row=!flex||!box.style.Is(L"flex-direction",L"column");
        float content=0;int visible=0;
        for(const auto& child:box.children)
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
                const float childWidth=NaturalWidth(*child);
                content=row?content+childWidth:std::max(content,childWidth);
                ++visible;
            }
        if(box.children.empty())content=TextWidth(box.node->InnerText(),box.style);
        if(flex&&row)content+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
        value=content+padding.left+padding.right+border.left+border.right;
    }
    else{
        const auto display=box.style.Get(L"display");const bool flex=display==L"flex"||display==L"inline-flex";const bool horizontal=IsInlineLevel(display)||display==L"table-row"||(flex&&!box.style.Is(L"flex-direction",L"column"));int visible=0;
        if(horizontal){
            float lineWidth=0;
            for(auto& c:box.children)if(c->visible&&!c->style.Is(L"position",L"absolute")&&!c->style.Is(L"position",L"fixed")){
                if(c->node->tag==L"br"){value=std::max(value,lineWidth);lineWidth=0;continue;}
                lineWidth+=NaturalWidth(*c);++visible;
            }
            value=std::max(value,lineWidth);
        }else{
            // The max-content width of normal block flow is the widest line,
            // not the widest individual inline child. Adjacent inline boxes
            // therefore contribute together until a block or <br> ends the
            // line. This also gives shrink-to-fit flex/grid items enough room
            // to keep an authored inline run on one line.
            float lineWidth=0;
            auto finishLine=[&]{value=std::max(value,lineWidth);lineWidth=0;};
            for(auto& c:box.children){
                if(!c->visible||c->style.Is(L"position",L"absolute")||
                   c->style.Is(L"position",L"fixed"))continue;
                ++visible;
                if(c->node->tag==L"br"){finishLine();continue;}
                if(IsInlineLevel(c->style.Get(L"display")))lineWidth+=NaturalWidth(*c);
                else{finishLine();value=std::max(value,NaturalWidth(*c));}
            }
            finishLine();
        }
        if(flex&&horizontal)value+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
        auto padding=EdgeValues(box.style,L"padding",500,500);auto border=BorderValues(box.style);value+=padding.left+padding.right+border.left+border.right;
    }
    value=Constrain(box.style,L"min-width",L"max-width",value,500,500);auto margin=EdgeValues(box.style,L"margin",500,500);return remember(value+margin.left+margin.right);
}

std::vector<float> ResolveTableColumns(const LayoutBox& table,const TableGridModel& model,
                                       float availableWidth,float viewportWidth){
    if(!model.columnCount)return {};
    std::vector<float> explicitWidths(model.columnCount),preferredWidths(model.columnCount);
    size_t columnIndex=0;
    std::function<void(const std::shared_ptr<Node>&)> collectColumns=
        [&](const std::shared_ptr<Node>& node){
            if(!node||columnIndex>=model.columnCount)return;
            if(node->tag==L"col"){
                const size_t span=TableSpan(node,L"span",1000);
                const auto styleWidth=node->inlineStyle.find(L"width");
                const auto authoredWidth=styleWidth==node->inlineStyle.end()?
                    node->Attribute(L"width"):styleWidth->second;
                const float width=authoredWidth.empty()?0.0f:
                    StyleSheet::Length(authoredWidth,availableWidth,viewportWidth,0);
                for(size_t offset=0;offset<span&&columnIndex<model.columnCount;
                    ++offset,++columnIndex)explicitWidths[columnIndex]=std::max(0.0f,width);
                return;
            }
            for(const auto& child:node->children)collectColumns(child);
        };
    collectColumns(table.node);

    const bool fixed=table.style.Is(L"table-layout",L"fixed");
    auto distributeDeficit=[&](std::vector<float>& widths,const TableCellEntry& cell,float required,
                               const std::vector<float>* reserved){
        const size_t end=std::min(model.columnCount,cell.column+cell.columnSpan);
        float current=0;
        for(size_t column=cell.column;column<end;++column)
            current+=std::max(widths[column],reserved?(*reserved)[column]:0.0f);
        const float deficit=required-current;
        if(deficit<=0||end<=cell.column)return;
        std::vector<size_t> flexible;
        for(size_t column=cell.column;column<end;++column)
            if(!reserved||(*reserved)[column]<=0)flexible.push_back(column);
        if(flexible.empty())for(size_t column=cell.column;column<end;++column)
            flexible.push_back(column);
        const float share=deficit/static_cast<float>(flexible.size());
        for(const auto column:flexible)widths[column]+=share;
    };

    for(const auto& cell:model.cells){
        if(fixed&&cell.row!=0)continue;
        const auto raw=Trim(cell.box->style.Get(L"width"));
        if(!raw.empty()&&raw!=L"auto")
            distributeDeficit(explicitWidths,cell,
                StyleSheet::Length(raw,availableWidth,viewportWidth,0),nullptr);
    }
    if(!fixed)for(const auto& cell:model.cells)
        distributeDeficit(preferredWidths,cell,NaturalWidth(*cell.box),&explicitWidths);

    std::vector<float> result=explicitWidths;
    float assigned=0,preferred=0;size_t automatic=0;
    for(size_t column=0;column<result.size();++column){
        assigned+=result[column];
        if(result[column]<=0){++automatic;preferred+=preferredWidths[column];}
    }
    if(automatic){
        const float remaining=std::max(0.0f,availableWidth-assigned);
        for(size_t column=0;column<result.size();++column)if(result[column]<=0)
            result[column]=preferred>0?remaining*preferredWidths[column]/preferred:
                remaining/static_cast<float>(automatic);
    }else if(assigned>0&&assigned<availableWidth){
        const float scale=availableWidth/assigned;
        for(auto& width:result)width*=scale;
    }
    return result;
}

float MinContentWidth(const LayoutBox& box){
    if(box.minimumWidthValid)return box.minimumWidth;
    auto remember=[&](float value){box.minimumWidth=value;box.minimumWidthValid=true;return value;};
    const auto margin=EdgeValues(box.style,L"margin",500,500);
    const auto overflow=box.style.Get(L"overflow-x",box.style.Get(L"overflow",L"visible"));
    const auto explicitMinimum=Trim(box.style.Get(L"min-width"));
    if(!explicitMinimum.empty()&&explicitMinimum!=L"auto"){
        const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=BorderValues(box.style);
        const float decoration=box.style.Is(L"box-sizing",L"border-box")?0.0f:padding.left+padding.right+border.left+border.right;
        return remember(std::max(0.0f,StyleSheet::Length(explicitMinimum,500,500,0)+decoration+margin.left+margin.right));
    }
    if(overflow!=L"visible"&&overflow!=L"clip")return remember(margin.left+margin.right);
    if(box.node->type==NodeType::Text){
        const auto whiteSpace=box.style.Get(L"white-space");
        const auto text=NormalizeText(box.node->text,whiteSpace,box.preserveLeadingWhitespace,box.preserveTrailingWhitespace);
        if(PreventsTextWrapping(whiteSpace))return remember(TextWidth(text,box.style));
        float longest=1;for(const auto& word:Words(text))longest=std::max(longest,TextWidth(word,box.style));return remember(longest);
    }
    if(box.node->tag==L"br")return remember(0.0f);
    if(box.node->tag==L"input"||box.node->tag==L"select"||box.node->tag==L"button")return remember(NaturalWidth(box));
    const auto display=box.style.Get(L"display");const bool flex=display==L"flex"||display==L"inline-flex";
    const bool horizontal=IsInlineLevel(display)||display==L"table-row"||(flex&&!box.style.Is(L"flex-direction",L"column"));
    const auto flexWrap=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
    const bool wraps=flex&&horizontal&&(flexWrap==L"wrap"||flexWrap==L"wrap-reverse");
    float value=0;int visible=0;
    for(const auto& child:box.children)if(child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
        const float childWidth=MinContentWidth(*child);
        value=horizontal&&!wraps?value+childWidth:std::max(value,childWidth);++visible;
    }
    if(flex&&horizontal&&!wraps)value+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
    const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=BorderValues(box.style);
    value+=padding.left+padding.right+border.left+border.right+margin.left+margin.right;
    const auto maximum=Trim(box.style.Get(L"max-width"));if(!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto")value=std::min(value,StyleSheet::Length(maximum,500,500,value));
    return remember(std::max(0.0f,value));
}

float NaturalGridHeight(const LayoutBox& box,float availableWidth);

float NaturalHeight(const LayoutBox& box,float availableWidth=500){
    if(box.naturalHeightValid&&std::abs(box.naturalHeightReference-availableWidth)<0.01f)return box.naturalHeight;
    auto remember=[&](float result){box.naturalHeightReference=availableWidth;box.naturalHeight=result;box.naturalHeightValid=true;return result;};
    auto height=box.style.Get(L"height");float value=0;
    if(!height.empty()&&height!=L"auto"&&height.find(L'%')==std::wstring::npos){
        value=StyleSheet::Length(height,500,500,20);
        if(!box.style.Is(L"box-sizing",L"border-box")){
            const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
            const auto border=BorderValues(box.style);
            value+=padding.top+padding.bottom+border.top+border.bottom;
        }
    }
    else if(box.node->tag==L"br")value=LineHeight(box.style);
    else if(box.node->tag==L"input"&&(box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio"))value=13;
    else if(box.node->tag==L"textarea"){
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        auto border=BorderValues(box.style);size_t rows=2;
        const auto rawRows=Trim(box.node->Attribute(L"rows"));
        if(!rawRows.empty())try{rows=std::max<size_t>(1,std::stoul(rawRows));}catch(...){}
        value=LineHeight(box.style)*rows+padding.top+padding.bottom+border.top+border.bottom;
    }
    else if(box.node->tag==L"input"||box.node->tag==L"select"||
            (box.node->tag==L"button"&&box.style.Get(L"display")!=L"grid"&&
             box.style.Get(L"display")!=L"inline-grid"&&
             box.style.Get(L"display")!=L"flex"&&box.style.Get(L"display")!=L"inline-flex")){
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);auto border=BorderValues(box.style);
        const auto text=box.node->tag==L"button"?box.node->InnerText():(box.node->tag==L"select"?SelectedOptionText(box.node):box.node->Attribute(L"value"));
        value=ControlLineHeight(text,box.style)+padding.top+padding.bottom+border.top+border.bottom;
    }
    else if(box.node->tag==L"svg"){
        const auto viewBox=SvgNumbers(box.node->Attribute(L"viewbox"));
        if(viewBox.size()==4&&viewBox[2]>0)value=availableWidth*viewBox[3]/viewBox[2];
        else value=StyleSheet::Length(box.node->Attribute(L"height"),availableWidth,availableWidth,150);
    }
    else if(box.node->type==NodeType::Text)value=TextHeight(box.node->text,box.style,availableWidth,
        box.preserveLeadingWhitespace,box.preserveTrailingWhitespace);
    else if(!box.children.empty()){
        const auto display=box.style.Get(L"display");const float rowGap=GapValue(box.style,false,availableWidth,availableWidth);
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);auto border=BorderValues(box.style);const float innerWidth=std::max(1.0f,availableWidth-padding.left-padding.right-border.left-border.right);
        if(display==L"table"){
            auto& mutableBox=const_cast<LayoutBox&>(box);
            const auto model=BuildTableGrid(mutableBox);
            const auto columns=ResolveTableColumns(box,model,innerWidth,availableWidth);
            const auto rows=ResolveTableRows(model,columns);
            for(const auto rowHeight:rows)value+=rowHeight;
        }else if(display==L"grid"){
            value=NaturalGridHeight(box,innerWidth);
        }else{
            const bool flex=display==L"flex"||display==L"inline-flex";
            const bool row=(flex&&!box.style.Is(L"flex-direction",L"column"))||
                (IsInlineLevel(display)&&!IsBlockifiedItem(box))||display==L"table-row";
            int visible=0;
            const auto flexWrap=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
            if(flex&&row&&(flexWrap==L"wrap"||flexWrap==L"wrap-reverse")){
                const float columnGap=GapValue(box.style,true,innerWidth,availableWidth);
                float lineWidth=0,lineHeight=0;size_t lineItems=0,lineCount=0;
                auto finishFlexLine=[&]{
                    if(!lineItems)return;
                    if(lineCount++)value+=rowGap;
                    value+=lineHeight;lineWidth=0;lineHeight=0;lineItems=0;
                };
                for(auto& child:box.children)if(child->visible&&
                    !child->style.Is(L"position",L"absolute")&&
                    !child->style.Is(L"position",L"fixed")){
                    const float childWidth=NaturalWidth(*child);
                    const float candidate=lineWidth+(lineItems?columnGap:0)+childWidth;
                    if(lineItems&&candidate>innerWidth+0.5f)finishFlexLine();
                    if(lineItems)lineWidth+=columnGap;
                    lineWidth+=childWidth;
                    lineHeight=std::max(lineHeight,NaturalHeight(*child,
                        std::max(1.0f,std::min(childWidth,innerWidth))));
                    ++lineItems;++visible;
                }
                finishFlexLine();
            }else if(row||flex){
                float lineHeight=0;
                for(auto& child:box.children)if(child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
                    if(row&&child->node->tag==L"br"){
                        value+=lineHeight>0?lineHeight:LineHeight(child->style);lineHeight=0;continue;
                    }
                    if(row)lineHeight=std::max(lineHeight,NaturalHeight(*child,innerWidth));
                    else value+=NaturalHeight(*child,innerWidth);
                    ++visible;
                }
                if(row)value+=lineHeight;
                if(flex&&!row)value+=rowGap*std::max(0,visible-1);
            }else{
                // Normal block flow groups adjacent inline boxes into lines.
                // A <br> flushes the current line, and consecutive breaks add
                // an empty line instead of behaving like a tall empty block.
                float lineWidth=0,lineHeight=0;
                auto flushLine=[&]{value+=lineHeight;lineWidth=0;lineHeight=0;};
                for(auto& child:box.children){
                    if(!child->visible||child->style.Is(L"position",L"absolute")||
                       child->style.Is(L"position",L"fixed"))continue;
                    if(child->node->tag==L"br"){
                        if(lineWidth>0||lineHeight>0)flushLine();
                        else value+=LineHeight(child->style);
                        continue;
                    }
                    if(IsInlineLevel(child->style.Get(L"display"))){
                        const float childWidth=NaturalWidth(*child);
                        if(lineWidth>0&&lineWidth+childWidth>innerWidth+0.5f)flushLine();
                        lineWidth+=std::min(childWidth,innerWidth);
                        const bool atomic=IsAtomicInlineLevel(*child);
                        float childHeight=NaturalHeight(*child,
                            std::max(1.0f,std::min(childWidth,innerWidth)));
                        if(!atomic&&child->node->type==NodeType::Element&&
                           !IsBlockifiedItem(*child)){
                            const auto childPadding=EdgeValues(child->style,L"padding",innerWidth,innerWidth);
                            const auto childBorder=BorderValues(child->style);
                            const auto childMargin=EdgeValues(child->style,L"margin",innerWidth,innerWidth);
                            childHeight=std::max(LineHeight(child->style),childHeight-
                                childPadding.top-childPadding.bottom-childBorder.top-childBorder.bottom-
                                childMargin.top-childMargin.bottom);
                        }
                        // Every CSS inline formatting context carries a strut
                        // with the containing block's font and line-height. A
                        // line made only from smaller inline descendants must
                        // therefore not collapse below the parent's line box.
                        lineHeight=std::max(LineHeight(box.style),std::max(lineHeight,
                            childHeight+(atomic?InlineFormattingDescent(box.style):0.0f)));
                    }else{
                        if(lineWidth>0||lineHeight>0)flushLine();
                        value+=NaturalHeight(*child,innerWidth);
                    }
                }
                if(lineWidth>0||lineHeight>0)flushLine();
            }
        }
        value+=padding.top+padding.bottom+border.top+border.bottom;
    }else value=0;
    // Percentage block-size constraints are indefinite during intrinsic
    // measurement. Resolve them later from a definite containing block rather
    // than from this routine's measurement fallback.
    value=ConstrainIntrinsicHeight(box.style,value,500);auto margin=EdgeValues(box.style,L"margin",availableWidth,availableWidth);return remember(value+margin.top+margin.bottom);
}

std::vector<float> ResolveTableRows(const TableGridModel& model,
                                    const std::vector<float>& columns){
    std::vector<float> rows(model.rows.size(),20.0f);
    for(size_t row=0;row<model.rows.size();++row){
        const auto raw=Trim(model.rows[row].box->style.Get(L"height"));
        if(!raw.empty()&&raw!=L"auto"&&raw.find(L'%')==std::wstring::npos)
            rows[row]=std::max(rows[row],StyleSheet::Length(raw,500,500,0));
    }
    auto cellWidth=[&](const TableCellEntry& cell){
        float width=0;
        const size_t end=std::min(columns.size(),cell.column+cell.columnSpan);
        for(size_t column=cell.column;column<end;++column)width+=columns[column];
        return width;
    };
    for(const auto& cell:model.cells)if(cell.rowSpan==1&&cell.row<rows.size())
        rows[cell.row]=std::max(rows[cell.row],NaturalHeight(*cell.box,cellWidth(cell)));
    for(const auto& cell:model.cells)if(cell.rowSpan>1&&cell.row<rows.size()){
        const size_t end=std::min(rows.size(),cell.row+cell.rowSpan);
        float current=0;
        for(size_t row=cell.row;row<end;++row)current+=rows[row];
        const float deficit=NaturalHeight(*cell.box,cellWidth(cell))-current;
        if(deficit<=0||end<=cell.row)continue;
        const float share=deficit/static_cast<float>(end-cell.row);
        for(size_t row=cell.row;row<end;++row)rows[row]+=share;
    }
    return rows;
}

LayoutRect PositionedRect(const LayoutBox& box,const LayoutRect& area,float viewportWidth,float viewportHeight){
    const auto leftRaw=box.style.Get(L"left"),rightRaw=box.style.Get(L"right");
    const auto topRaw=box.style.Get(L"top"),bottomRaw=box.style.Get(L"bottom");
    const float left=StyleSheet::Length(leftRaw,area.width,viewportWidth,0);
    const float right=StyleSheet::Length(rightRaw,area.width,viewportWidth,0);
    const float top=StyleSheet::Length(topRaw,area.height,viewportHeight,0);
    const float bottom=StyleSheet::Length(bottomRaw,area.height,viewportHeight,0);
    const auto widthRaw=box.style.Get(L"width"),heightRaw=box.style.Get(L"height");
    float width=NaturalWidth(box);
    if(!widthRaw.empty()&&widthRaw!=L"auto")width=StyleSheet::Length(widthRaw,area.width,viewportWidth,width);
    else if(!leftRaw.empty()&&!rightRaw.empty())width=std::max(0.0f,area.width-left-right);
    float height=NaturalHeight(box,width);
    if(!heightRaw.empty()&&heightRaw!=L"auto")height=StyleSheet::Length(heightRaw,area.height,viewportHeight,height);
    else if(!topRaw.empty()&&!bottomRaw.empty())height=std::max(0.0f,area.height-top-bottom);
    const float x=!leftRaw.empty()?area.x+left:(!rightRaw.empty()?area.x+area.width-right-width:area.x);
    const float y=!topRaw.empty()?area.y+top:(!bottomRaw.empty()?area.y+area.height-bottom-height:area.y);
    return {x,y,std::max(0.0f,width),std::max(0.0f,height)};
}

LayoutRect AbsoluteContainingBlock(const LayoutBox& box){
    // CSS establishes an absolutely positioned descendant's containing block
    // from the ancestor's padding box, not its content box.  Derive it from
    // the final border box so percentages and opposing insets include authored
    // padding while still excluding the border at every monitor DPI.
    const auto border=BorderValues(box.style);
    return {box.rect.x+border.left,box.rect.y+border.top,
        std::max(0.0f,box.rect.width-border.left-border.right),
        std::max(0.0f,box.rect.height-border.top-border.bottom)};
}

struct GridAreaDefinition {
    std::wstring name;
    size_t row = 0, column = 0, rowSpan = 1, columnSpan = 1;
};

struct GridItemPlacement {
    LayoutBox* box = nullptr;
    size_t row = 0, column = 0, rowSpan = 1, columnSpan = 1;
};

enum class GridLineKind { Automatic, Line, Span };
struct GridLineValue {
    GridLineKind kind = GridLineKind::Automatic;
    int line = 0;
    size_t span = 1;
};
struct GridAxisPlacement {
    bool definite = false;
    size_t start = 0, span = 1;
};

std::vector<std::wstring> SplitGridPlacement(const std::wstring& source) {
    std::vector<std::wstring> result;size_t start=0;int nesting=0;
    for(size_t index=0;index<source.size();++index){
        if(source[index]==L'(')++nesting;
        else if(source[index]==L')')--nesting;
        else if(source[index]==L'/'&&nesting==0){
            result.push_back(Trim(source.substr(start,index-start)));start=index+1;
        }
    }
    result.push_back(Trim(source.substr(start)));return result;
}

bool GridInteger(const std::wstring& source,int& value) {
    const auto trimmed=Trim(source);
    try{
        size_t used=0;const int parsed=std::stoi(trimmed,&used);
        if(used!=trimmed.size()||parsed==0)return false;
        value=parsed;return true;
    }catch(...){return false;}
}

GridLineValue ParseGridLine(const std::wstring& source) {
    const auto value=ToLower(Trim(source));
    if(value.empty()||value==L"auto")return {};
    const auto words=Words(value);
    if(!words.empty()&&words.front()==L"span"){
        GridLineValue result;result.kind=GridLineKind::Span;
        for(size_t index=1;index<words.size();++index){
            int span=0;if(GridInteger(words[index],span)&&span>0){result.span=static_cast<size_t>(span);break;}
        }
        return result;
    }
    int line=0;if(GridInteger(value,line))return {GridLineKind::Line,line,1};
    return {};
}

size_t ResolveGridLine(int line,size_t explicitTracks) {
    if(line>0)return static_cast<size_t>(line-1);
    const auto resolved=static_cast<long long>(explicitTracks)+1+line;
    return static_cast<size_t>(std::max<long long>(0,resolved));
}

GridAxisPlacement ResolveGridAxis(const std::wstring& startSource,
                                  const std::wstring& endSource,size_t explicitTracks) {
    const auto start=ParseGridLine(startSource),end=ParseGridLine(endSource);
    GridAxisPlacement result;
    if(start.kind==GridLineKind::Line){
        const size_t first=ResolveGridLine(start.line,explicitTracks);
        result.definite=true;result.start=first;
        if(end.kind==GridLineKind::Line){
            const size_t last=ResolveGridLine(end.line,explicitTracks);
            result.start=std::min(first,last);
            result.span=std::max<size_t>(1,first>last?first-last:last-first);
        }else if(end.kind==GridLineKind::Span)result.span=end.span;
        return result;
    }
    if(end.kind==GridLineKind::Line){
        const size_t last=ResolveGridLine(end.line,explicitTracks);
        result.definite=true;result.span=start.kind==GridLineKind::Span?start.span:1;
        result.start=last>result.span?last-result.span:0;return result;
    }
    if(start.kind==GridLineKind::Span)result.span=start.span;
    else if(end.kind==GridLineKind::Span)result.span=end.span;
    return result;
}

GridAxisPlacement ItemGridAxis(const LayoutBox& box,bool rows,size_t explicitTracks,
                               const std::vector<std::wstring>& areaParts) {
    std::wstring start,end;
    const size_t startIndex=rows?0:1,endIndex=rows?2:3;
    if(startIndex<areaParts.size())start=areaParts[startIndex];
    if(endIndex<areaParts.size())end=areaParts[endIndex];
    const auto shorthand=Trim(box.style.Get(rows?L"grid-row":L"grid-column"));
    if(!shorthand.empty()){
        const auto parts=SplitGridPlacement(shorthand);
        start=parts.empty()?L"auto":parts[0];end=parts.size()>1?parts[1]:L"auto";
    }
    const auto startLonghand=Trim(box.style.Get(rows?L"grid-row-start":L"grid-column-start"));
    const auto endLonghand=Trim(box.style.Get(rows?L"grid-row-end":L"grid-column-end"));
    if(!startLonghand.empty())start=startLonghand;
    if(!endLonghand.empty())end=endLonghand;
    return ResolveGridAxis(start,end,explicitTracks);
}

struct GridTrackSizing {
    float base = 0;
    float limit = 0;
    float fraction = 0;
    bool intrinsic = false;
    bool stretch = false;
};

using GridTrackDefinitions=std::vector<std::wstring>;
std::shared_ptr<const GridTrackDefinitions> ExpandGridTracks(
    const std::wstring& definition,const LayoutBox& box,float reference) {
    const bool contextDependent=definition.find(L"repeat(auto-fit")!=std::wstring::npos||
        definition.find(L"repeat(auto-fill")!=std::wstring::npos;
    static thread_local FastMap<std::wstring,std::shared_ptr<const GridTrackDefinitions>> cache;
    if(!contextDependent)
        if(const auto found=cache.find(definition);found!=cache.end())return found->second;
    auto result=std::make_shared<GridTrackDefinitions>();
    for(const auto& token:Words(definition)){
        if(token.rfind(L"repeat(",0)!=0||token.size()<9||token.back()!=L')'){
            if(!token.empty()&&token!=L"none")result->push_back(token);
            continue;
        }
        const auto inside=token.substr(7,token.size()-8);
        size_t comma=std::wstring::npos;int nesting=0;
        for(size_t i=0;i<inside.size();++i){
            if(inside[i]==L'(')++nesting;
            else if(inside[i]==L')')--nesting;
            else if(inside[i]==L','&&nesting==0){comma=i;break;}
        }
        if(comma==std::wstring::npos)continue;
        const auto countToken=Trim(inside.substr(0,comma));
        const auto repeated=Words(Trim(inside.substr(comma+1)));
        size_t count=0;
        if(countToken==L"auto-fit"||countToken==L"auto-fill")count=GridColumnCount(box,reference);
        else try{count=std::max<size_t>(1,std::stoul(countToken));}catch(...){count=1;}
        for(size_t repeat=0;repeat<count;++repeat)
            result->insert(result->end(),repeated.begin(),repeated.end());
    }
    if(!contextDependent){if(cache.size()>=256)cache.clear();cache.emplace(definition,result);}
    return result;
}

std::vector<GridAreaDefinition> ParseGridAreas(const std::wstring& definition,
                                               size_t& rowCount,size_t& columnCount) {
    std::vector<std::vector<std::wstring>> rows;
    wchar_t quote=0;std::wstring current;
    for(const wchar_t c:definition){
        if(!quote&&(c==L'\''||c==L'"')){quote=c;current.clear();}
        else if(quote&&c==quote){rows.push_back(Words(current));quote=0;}
        else if(quote)current+=c;
    }
    rowCount=rows.size();columnCount=0;
    for(const auto& row:rows)columnCount=std::max(columnCount,row.size());
    std::vector<GridAreaDefinition> areas;
    for(size_t row=0;row<rows.size();++row)for(size_t column=0;column<rows[row].size();++column){
        const auto& name=rows[row][column];if(name.empty()||name==L".")continue;
        auto found=std::find_if(areas.begin(),areas.end(),[&](const auto& area){return area.name==name;});
        if(found==areas.end())areas.push_back({name,row,column,1,1});
        else{
            const auto lastRow=std::max(found->row+found->rowSpan,row+1);
            const auto lastColumn=std::max(found->column+found->columnSpan,column+1);
            found->row=std::min(found->row,row);found->column=std::min(found->column,column);
            found->rowSpan=lastRow-found->row;found->columnSpan=lastColumn-found->column;
        }
    }
    return areas;
}

std::vector<GridItemPlacement> PlaceGridItems(const LayoutBox& box,
    const std::vector<GridAreaDefinition>& areas,size_t explicitRows,size_t& columnCount,
    size_t& usedRows) {
    struct Request { LayoutBox* box=nullptr;GridAxisPlacement row,column; };
    const size_t explicitColumns=columnCount;
    std::vector<Request> requests;requests.reserve(box.children.size());
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||
           child->style.Is(L"position",L"fixed"))continue;
        const auto areaValue=Trim(child->style.Get(L"grid-area"));
        const auto named=std::find_if(areas.begin(),areas.end(),
            [&](const auto& value){return value.name==areaValue;});
        Request request;request.box=child.get();
        if(named!=areas.end()){
            request.row={true,named->row,named->rowSpan};
            request.column={true,named->column,named->columnSpan};
        }else{
            const auto areaParts=SplitGridPlacement(areaValue);
            request.row=ItemGridAxis(*child,true,explicitRows,areaParts);
            request.column=ItemGridAxis(*child,false,explicitColumns,areaParts);
        }
        if(request.column.definite)
            columnCount=std::max(columnCount,request.column.start+request.column.span);
        else columnCount=std::max(columnCount,request.column.span);
        requests.push_back(request);
    }
    columnCount=std::max<size_t>(1,columnCount);
    std::vector<GridItemPlacement> items;items.reserve(requests.size());
    std::vector<std::vector<bool>> occupied(explicitRows,std::vector<bool>(columnCount,false));
    auto ensureRows=[&](size_t count){
        while(occupied.size()<count)occupied.push_back(std::vector<bool>(columnCount,false));
    };
    auto canPlace=[&](size_t row,size_t column,size_t rowSpan,size_t columnSpan){
        if(column+columnSpan>columnCount)return false;
        for(size_t y=row;y<row+rowSpan;++y)for(size_t x=column;x<column+columnSpan;++x)
            if(y<occupied.size()&&occupied[y][x])return false;
        return true;
    };
    auto place=[&](const Request& request,size_t row,size_t column){
        ensureRows(row+request.row.span);
        items.push_back({request.box,row,column,request.row.span,request.column.span});
        for(size_t y=row;y<row+request.row.span;++y)
            for(size_t x=column;x<column+request.column.span;++x)occupied[y][x]=true;
    };

    // Explicitly positioned items may overlap each other, but their occupied
    // cells still steer the later auto-placement phases.
    for(const auto& request:requests)if(request.row.definite&&request.column.definite)
        place(request,request.row.start,request.column.start);
    for(const auto& request:requests)if(request.row.definite&&!request.column.definite){
        size_t column=0;
        while(column+request.column.span<=columnCount&&
              !canPlace(request.row.start,column,request.row.span,request.column.span))++column;
        if(column+request.column.span>columnCount){
            const size_t oldCount=columnCount;columnCount+=request.column.span;
            for(auto& row:occupied)row.resize(columnCount,false);column=oldCount;
        }
        place(request,request.row.start,column);
    }
    for(const auto& request:requests)if(!request.row.definite&&request.column.definite){
        size_t row=0;while(!canPlace(row,request.column.start,request.row.span,request.column.span))++row;
        place(request,row,request.column.start);
    }
    size_t cursor=0;
    for(const auto& request:requests)if(!request.row.definite&&!request.column.definite){
        while(true){
            const size_t row=cursor/columnCount,column=cursor%columnCount;++cursor;
            if(column+request.column.span>columnCount)continue;
            if(canPlace(row,column,request.row.span,request.column.span)){
                place(request,row,column);break;
            }
        }
    }
    usedRows=occupied.size();
    return items;
}

GridTrackSizing ParseGridTrack(const std::wstring& source,float reference,float viewport) {
    const bool contextIndependent=source.find(L'%')==std::wstring::npos&&
        source.find(L"vh")==std::wstring::npos&&source.find(L"vw")==std::wstring::npos&&
        source.find(L"em")==std::wstring::npos&&source.find(L"calc(")==std::wstring::npos&&
        source.find(L"clamp(")==std::wstring::npos;
    static thread_local FastMap<std::wstring,GridTrackSizing> cache;
    if(contextIndependent)
        if(const auto found=cache.find(source);found!=cache.end())return found->second;
    const auto token=ToLower(Trim(source));
    GridTrackSizing track;track.limit=std::numeric_limits<float>::infinity();
    auto remember=[&](const GridTrackSizing& value){
        if(contextIndependent){if(cache.size()>=256)cache.clear();cache.emplace(source,value);}
        return value;
    };
    auto fraction=[](const std::wstring& value){try{return std::stof(value);}catch(...){return 1.0f;}};
    auto intrinsic=[](const std::wstring& value){return value==L"auto"||value==L"min-content"||value==L"max-content";};
    if(token.rfind(L"minmax(",0)==0&&token.size()>8&&token.back()==L')'){
        const auto values=CommaSeparated(token.substr(7,token.size()-8));
        if(values.size()==2){
            const auto minimum=Trim(values[0]),maximum=Trim(values[1]);
            if(intrinsic(minimum))track.intrinsic=true;
            else track.base=std::max(0.0f,StyleSheet::Length(minimum,reference,viewport,0));
            if(maximum.find(L"fr")!=std::wstring::npos){track.fraction=std::max(0.0f,fraction(maximum));}
            else if(intrinsic(maximum)){track.intrinsic=true;track.stretch=maximum==L"auto";}
            else track.limit=std::max(track.base,StyleSheet::Length(maximum,reference,viewport,track.base));
            return remember(track);
        }
    }
    if(token.find(L"fr")!=std::wstring::npos){track.fraction=std::max(0.0f,fraction(token));track.intrinsic=true;return remember(track);}
    if(intrinsic(token)){track.intrinsic=true;track.stretch=token==L"auto";return remember(track);}
    track.base=std::max(0.0f,StyleSheet::Length(token,reference,viewport,0));track.limit=track.base;
    return remember(track);
}

std::vector<float> ResolveGridTracks(const std::vector<std::wstring>& definitions,
                                     size_t requiredCount,float available,float gap,float viewport,
                                     const std::vector<GridItemPlacement>& items,bool columns,
                                     const std::vector<float>& oppositeSizes,bool definiteAvailable,
                                     bool stretchAutoTracks=true) {
    const size_t count=std::max<size_t>(1,std::max(requiredCount,definitions.size()));
    std::vector<GridTrackSizing> tracks;tracks.reserve(count);
    for(size_t i=0;i<count;++i)
        tracks.push_back(ParseGridTrack(i<definitions.size()?definitions[i]:L"auto",available,viewport));
    std::vector<bool> shrinkableAutoTracks(count,false);

    auto spanSize=[&](const GridItemPlacement& item){
        const auto start=columns?item.row:item.column;
        const auto span=columns?item.rowSpan:item.columnSpan;
        float result=gap*std::max(0,static_cast<int>(span)-1);
        for(size_t index=0;index<span&&start+index<oppositeSizes.size();++index)result+=oppositeSizes[start+index];
        return std::max(1.0f,result);
    };
    for(const auto& item:items){
        const auto start=columns?item.column:item.row;
        const auto span=columns?item.columnSpan:item.rowSpan;
        if(start>=tracks.size())continue;
        bool spansFlexibleTrack=false,spansStretchTrack=false;
        for(size_t index=0;index<span&&start+index<tracks.size();++index){
            spansFlexibleTrack=spansFlexibleTrack||tracks[start+index].fraction>0;
            spansStretchTrack=spansStretchTrack||tracks[start+index].stretch;
        }
        // Bare fr and auto tracks have automatic minimums.  In a definite grid
        // their item contribution is the min-content size, not max-content.
        // Using NaturalWidth here lets a newly revealed no-wrap descendant
        // expand an outer implicit auto track past the grid container; a later
        // viewport relayout then appears to be what made the nested grid responsive.
        float contribution=columns&&definiteAvailable&&
            (spansFlexibleTrack||spansStretchTrack)?
            MinContentWidth(*item.box):
            (columns?NaturalWidth(*item.box):NaturalHeight(*item.box,spanSize(item)));
        // A percentage inline size is resolved from the finished grid area. It
        // is indefinite while intrinsic track contributions are collected, so
        // it must not inject the control's fallback intrinsic width into an fr
        // track (for example, a width:100% select inside a 140px grid cell).
        if(columns){
            const auto itemWidth=Trim(item.box->style.Get(L"width"));
            if(itemWidth.find(L'%')!=std::wstring::npos)contribution=0;
            const bool automaticSize=itemWidth.empty()||itemWidth==L"auto"||
                itemWidth.find(L'%')!=std::wstring::npos;
            const auto minimum=Trim(item.box->style.Get(L"min-width"));
            const auto overflow=item.box->style.Get(L"overflow-x",item.box->style.Get(L"overflow",L"visible"));
            // A zero automatic minimum applies only while the item size is
            // automatic. A definite width/height remains its track sizing
            // contribution even when the item clips overflowing descendants.
            // A definite grid container also does not by itself make an
            // intrinsic track flexible: non-stretched auto tracks retain the
            // content contribution, fr tracks use the zero minimum immediately,
            // and stretched auto tracks shrink only if their combined natural
            // size actually exceeds the definite grid area.
            const bool zeroAutomaticMinimum=automaticSize&&
                ((!minimum.empty()&&StyleSheet::Length(minimum,available,viewport,1)==0)||
                 (overflow!=L"visible"&&overflow!=L"clip"));
            if(zeroAutomaticMinimum){
                if(spansFlexibleTrack)contribution=0;
                else if(definiteAvailable&&stretchAutoTracks)
                    for(size_t index=0;index<span&&start+index<tracks.size();++index)
                        shrinkableAutoTracks[start+index]=tracks[start+index].stretch;
            }
        }else{
            const auto itemHeight=Trim(item.box->style.Get(L"height"));
            if(itemHeight.find(L'%')!=std::wstring::npos)contribution=0;
            const bool automaticSize=itemHeight.empty()||itemHeight==L"auto"||
                itemHeight.find(L'%')!=std::wstring::npos;
            const auto minimum=Trim(item.box->style.Get(L"min-height"));
            const auto overflow=item.box->style.Get(L"overflow-y",item.box->style.Get(L"overflow",L"visible"));
            const bool zeroAutomaticMinimum=automaticSize&&
                ((!minimum.empty()&&StyleSheet::Length(minimum,available,viewport,1)==0)||
                 (overflow!=L"visible"&&overflow!=L"clip"));
            if(zeroAutomaticMinimum){
                if(spansFlexibleTrack)contribution=0;
                else if(definiteAvailable&&stretchAutoTracks)
                    for(size_t index=0;index<span&&start+index<tracks.size();++index)
                        shrinkableAutoTracks[start+index]=tracks[start+index].stretch;
            }
        }
        float occupied=gap*std::max(0,static_cast<int>(span)-1);
        for(size_t index=0;index<span&&start+index<tracks.size();++index)occupied+=tracks[start+index].base;
        float deficit=std::max(0.0f,contribution-occupied);
        if(deficit<=0)continue;
        std::vector<size_t> eligible;
        for(size_t index=0;index<span&&start+index<tracks.size();++index){
            const size_t trackIndex=start+index;
            if(tracks[trackIndex].intrinsic)eligible.push_back(trackIndex);
        }
        if(eligible.empty())continue;
        for(size_t remaining=eligible.size();remaining&&!eligible.empty()&&deficit>0.01f;){
            const float share=deficit/static_cast<float>(remaining);bool removed=false;
            for(auto it=eligible.begin();it!=eligible.end();){
                auto& track=tracks[*it];const float room=track.limit-track.base;
                const float growth=std::min(share,std::max(0.0f,room));track.base+=growth;deficit-=growth;
                if(room<=share+0.01f){it=eligible.erase(it);--remaining;removed=true;}else ++it;
            }
            if(!removed)break;
        }
        if(!eligible.empty()&&deficit>0){const float share=deficit/eligible.size();for(const auto index:eligible)tracks[index].base+=share;}
    }

    const float gaps=gap*std::max(0,static_cast<int>(tracks.size())-1);
    auto used=[&](){float total=gaps;for(const auto& track:tracks)total+=track.base;return total;};
    float overflow=std::max(0.0f,used()-available);
    std::vector<size_t> shrinkable;
    for(size_t index=0;index<tracks.size();++index)
        if(shrinkableAutoTracks[index]&&tracks[index].base>0.01f)shrinkable.push_back(index);
    while(overflow>0.01f&&!shrinkable.empty()){
        const float share=overflow/static_cast<float>(shrinkable.size());
        bool removed=false;
        for(auto it=shrinkable.begin();it!=shrinkable.end();){
            auto& track=tracks[*it];
            const float reduction=std::min(share,track.base);
            track.base-=reduction;overflow-=reduction;
            if(track.base<=0.01f){track.base=0;it=shrinkable.erase(it);removed=true;}
            else ++it;
        }
        if(!removed)break;
    }
    float free=std::max(0.0f,available-used());
    std::vector<size_t> capped;
    for(size_t i=0;i<tracks.size();++i)if(std::isfinite(tracks[i].limit)&&tracks[i].limit>tracks[i].base+0.01f)capped.push_back(i);
    while(free>0.01f&&!capped.empty()){
        const float share=free/capped.size();bool removed=false;
        for(auto it=capped.begin();it!=capped.end();){auto& track=tracks[*it];const float growth=std::min(share,track.limit-track.base);track.base+=growth;free-=growth;if(track.limit-track.base<=0.01f){it=capped.erase(it);removed=true;}else ++it;}
        if(!removed)break;
    }
    float totalFraction=0;for(const auto& track:tracks)totalFraction+=track.fraction;
    if(free>0.01f&&totalFraction>0){
        // An fr track's base is its automatic minimum, not a head start that
        // is added to an equal share of the remaining space.  Resolve one flex
        // fraction from the whole flexible area and freeze only tracks whose
        // minimum is larger than that share.  This keeps equal 1fr columns
        // equal whenever all of their min-content sizes fit, as CSS Grid does.
        std::vector<size_t> flexible;
        float flexibleSpace=available-gaps;
        for(size_t index=0;index<tracks.size();++index){
            if(tracks[index].fraction>0)flexible.push_back(index);
            else flexibleSpace-=tracks[index].base;
        }
        flexibleSpace=std::max(0.0f,flexibleSpace);
        std::vector<bool> frozen(tracks.size(),false);
        while(!flexible.empty()){
            float frozenSize=0,totalActiveFraction=0;
            for(size_t index=0;index<tracks.size();++index)
                if(frozen[index])frozenSize+=tracks[index].base;
            for(const auto index:flexible)totalActiveFraction+=tracks[index].fraction;
            if(totalActiveFraction<=0)break;
            const float fractionSize=std::max(0.0f,flexibleSpace-frozenSize)/
                std::max(1.0f,totalActiveFraction);
            bool frozeTrack=false;
            for(auto it=flexible.begin();it!=flexible.end();){
                auto& track=tracks[*it];
                if(track.base>fractionSize*track.fraction+0.01f){
                    frozen[*it]=true;it=flexible.erase(it);frozeTrack=true;
                }else ++it;
            }
            if(frozeTrack)continue;
            for(const auto index:flexible)
                tracks[index].base=fractionSize*tracks[index].fraction;
            break;
        }
    }else if(free>0.01f&&stretchAutoTracks){
        size_t stretchCount=0;for(const auto& candidate:tracks)if(candidate.stretch)++stretchCount;
        if(stretchCount)for(auto& stretchTrack:tracks)if(stretchTrack.stretch)stretchTrack.base+=free/stretchCount;
    }

    std::vector<float> result;result.reserve(tracks.size());
    for(const auto& track:tracks)result.push_back(std::max(0.0f,track.base));
    return result;
}

struct GridContentDistribution {
    float offset = 0;
    float extraGap = 0;
};

std::wstring GridContentAlignment(const std::wstring& source) {
    const auto parts=Words(ToLower(Trim(source)));
    for(auto it=parts.rbegin();it!=parts.rend();++it)
        if(*it!=L"safe"&&*it!=L"unsafe")return *it;
    return L"normal";
}

bool StretchesGridAutoTracks(const std::wstring& source) {
    const auto alignment=GridContentAlignment(source);
    return alignment.empty()||alignment==L"normal"||alignment==L"stretch";
}

GridContentDistribution DistributeGridContent(const std::wstring& source,float available,
                                               const std::vector<float>& tracks,float gap) {
    GridContentDistribution result;
    if(tracks.empty())return result;
    float occupied=gap*std::max(0,static_cast<int>(tracks.size())-1);
    for(const float track:tracks)occupied+=track;
    const float free=std::max(0.0f,available-occupied);
    if(free<=0.01f)return result;

    const auto alignment=GridContentAlignment(source);
    if(alignment==L"center")result.offset=free/2;
    else if(alignment==L"end"||alignment==L"flex-end")result.offset=free;
    else if(alignment==L"space-between"){
        if(tracks.size()>1)result.extraGap=free/static_cast<float>(tracks.size()-1);
    }else if(alignment==L"space-around"){
        result.extraGap=free/static_cast<float>(tracks.size());
        result.offset=result.extraGap/2;
    }else if(alignment==L"space-evenly"){
        result.extraGap=free/static_cast<float>(tracks.size()+1);
        result.offset=result.extraGap;
    }
    return result;
}

float NaturalGridHeight(const LayoutBox& box,float availableWidth) {
    const auto columnDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-columns",L"1fr"),box,availableWidth);
    const auto rowDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-rows"),box,0);
    size_t areaRows=0,areaColumns=0;const auto areas=ParseGridAreas(box.style.Get(L"grid-template-areas"),areaRows,areaColumns);
    size_t columnCount=std::max<size_t>(1,std::max(columnDefinitions->size(),areaColumns));
    const size_t explicitRows=std::max(areaRows,rowDefinitions->size());
    size_t usedRows=0;const auto items=PlaceGridItems(box,areas,explicitRows,columnCount,usedRows);
    const size_t rowCount=std::max<size_t>(1,std::max(rowDefinitions->size(),usedRows));
    const float columnGap=GapValue(box.style,true,availableWidth,availableWidth);
    const float rowGap=GapValue(box.style,false,availableWidth,availableWidth);
    const std::vector<float> provisionalRows(rowCount,20.0f);
    std::function<bool(const LayoutBox&)> widthIndependentHeight=[&](const LayoutBox& item){
        const auto explicitHeight=Trim(item.style.Get(L"height"));
        if(!explicitHeight.empty()&&explicitHeight!=L"auto"&&explicitHeight.find(L'%')==std::wstring::npos)return true;
        if(item.node->type==NodeType::Text){
            return PreventsTextWrapping(item.style.Get(L"white-space"));
        }
        if(item.node->tag==L"input"||item.node->tag==L"select"||item.node->tag==L"button"||item.node->tag==L"br")return true;
        if(item.node->tag==L"svg")return false;
        for(const auto& child:item.children)
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&
               !child->style.Is(L"position",L"fixed")&&!widthIndependentHeight(*child))return false;
        return true;
    };
    const bool heightDoesNotDependOnColumns=std::all_of(items.begin(),items.end(),
        [&](const GridItemPlacement& item){return widthIndependentHeight(*item.box);});
    const auto columns=heightDoesNotDependOnColumns?
        std::vector<float>(columnCount,std::max(1.0f,(availableWidth-columnGap*std::max(0,static_cast<int>(columnCount)-1))/columnCount)):
        ResolveGridTracks(*columnDefinitions,columnCount,availableWidth,columnGap,availableWidth,items,true,provisionalRows,true);
    const auto rows=ResolveGridTracks(*rowDefinitions,rowCount,0,rowGap,availableWidth,items,false,columns,false);
    float height=rowGap*std::max(0,static_cast<int>(rows.size())-1);for(const float row:rows)height+=row;
    return height;
}

unsigned int BackgroundColor(const ComputedStyle& style) {
    auto value=style.Get(L"background-color");if(value.empty())value=style.Get(L"background");
    if(ToLower(Trim(value))==L"currentcolor")value=style.Get(L"color",L"#000000");
    if(value.find(L"gradient")!=std::wstring::npos){
        // CSS backgrounds are painted back-to-front. If gradients are not yet
        // available, preserve the last authored solid layer as the fallback
        // instead of replacing the whole background with the first color stop.
        const auto layers=CommaSeparated(value);
        constexpr unsigned int invalid=0x01020304u;
        for(auto it=layers.rbegin();it!=layers.rend();++it){
            if(it->find(L"gradient")!=std::wstring::npos)continue;
            const auto direct=StyleSheet::Color(*it,invalid);
            if(direct!=invalid)return direct;
            const auto hash=it->find(L'#');
            if(hash!=std::wstring::npos){size_t end=hash+1;while(end<it->size()&&std::iswxdigit((*it)[end]))++end;return StyleSheet::Color(it->substr(hash,end-hash),0);}
            const auto rgb=it->find(L"rgb");
            if(rgb!=std::wstring::npos){const auto end=it->find(L')',rgb);if(end!=std::wstring::npos)return StyleSheet::Color(it->substr(rgb,end-rgb+1),0);}
        }
        // Gradient layers are painted separately and must stay transparent
        // here. Flattening their first stop changes semi-transparent gradients
        // into an opaque wash before the real layers are composited.
        return 0;
    }
    constexpr unsigned int invalid=0x01020304u;
    const auto direct=StyleSheet::Color(value,invalid);if(direct!=invalid)return direct;
    for(auto token:Words(value)){
        while(!token.empty()&&(token.back()==L','||token.back()==L';'))token.pop_back();
        const auto color=StyleSheet::Color(token,invalid);if(color!=invalid)return color;
    }
    return 0;
}

unsigned int DeclarationColor(const std::wstring& value,unsigned int fallback){
    constexpr unsigned int invalid=0x01020304u;const auto direct=StyleSheet::Color(value,invalid);if(direct!=invalid)return direct;
    const auto lowered=ToLower(value);if(lowered==L"none")return 0;
    auto tokens=Words(value);for(auto it=tokens.rbegin();it!=tokens.rend();++it){
        const auto color=StyleSheet::Color(*it,invalid);if(color!=invalid)return color;
    }
    return fallback;
}

unsigned int BorderColor(const ComputedStyle& style,const std::wstring& side){
    auto value=style.Get(L"border-"+side+L"-color");if(value.empty())value=style.Get(L"border-color");if(value.empty())value=style.Get(L"border-"+side);if(value.empty())value=style.Get(L"border");const auto lowered=ToLower(value);const auto current=lowered.find(L"currentcolor");if(current!=std::wstring::npos)value.replace(current,12,style.Get(L"color",L"#000000"));return DeclarationColor(value,0xffd1d5db);
}

bool IsPaintOnlyProperty(const std::wstring& name){
    return name==L"background"||name==L"background-color"||name==L"color"||
           name==L"border-color"||name==L"border-top-color"||name==L"border-right-color"||
           name==L"border-bottom-color"||name==L"border-left-color"||name==L"box-shadow"||
           name==L"opacity"||name==L"cursor"||name==L"outline"||name==L"outline-color"||
           name==L"text-decoration"||name==L"text-decoration-line"||
           name==L"text-decoration-color"||name==L"accent-color"||
           name==L"caret-color"||name==L"fill"||name==L"stroke";
}

bool HasLayoutStyleChange(const ComputedStyle& before,const ComputedStyle& after){
    for(const auto& item:*before.values){const auto found=after.values->find(item.first);if((found==after.values->end()||found->second!=item.second)&&!IsPaintOnlyProperty(item.first))return true;}
    for(const auto& item:*after.values){const auto found=before.values->find(item.first);if((found==before.values->end()||found->second!=item.second)&&!IsPaintOnlyProperty(item.first))return true;}
    return false;
}

D2D1_COLOR_F D2DColor(unsigned int color) {
    return D2D1::ColorF(((color>>16)&255)/255.0f,((color>>8)&255)/255.0f,(color&255)/255.0f,((color>>24)&255)/255.0f);
}

D2D1_RECT_F PixelAlignedRect(const LayoutRect& rect);

struct RadialGradientStop {
    unsigned int color = 0;
    float position = -1;
};

bool ParseRadialGradientStop(const std::wstring& source,RadialGradientStop& stop) {
    const auto value=Trim(source);if(value.empty())return false;
    std::wstring colorText,positionText;
    const auto lowered=ToLower(value);
    if(lowered.rfind(L"rgb(",0)==0||lowered.rfind(L"rgba(",0)==0){
        const auto close=value.find(L')');if(close==std::wstring::npos)return false;
        colorText=value.substr(0,close+1);positionText=Trim(value.substr(close+1));
    }else{
        const auto tokens=Words(value);if(tokens.empty())return false;
        colorText=tokens.front();if(tokens.size()>1)positionText=tokens.back();
    }
    constexpr unsigned int invalid=0x01020304u;
    stop.color=StyleSheet::Color(colorText,invalid);if(stop.color==invalid)return false;
    if(!positionText.empty()){
        try{
            size_t used=0;const float parsed=std::stof(positionText,&used);
            stop.position=positionText.find(L'%',used)!=std::wstring::npos?parsed/100.0f:parsed;
        }catch(...){stop.position=-1;}
    }
    return true;
}

void NormalizeGradientStops(std::vector<RadialGradientStop>& stops) {
    if(stops.empty())return;
    if(stops.front().position<0)stops.front().position=0;
    if(stops.back().position<0)stops.back().position=1;
    size_t begin=0;
    while(begin+1<stops.size()){
        size_t end=begin+1;while(end<stops.size()&&stops[end].position<0)++end;
        if(end>=stops.size())break;
        const float from=stops[begin].position,to=std::max(from,stops[end].position);
        for(size_t index=begin+1;index<end;++index)
            stops[index].position=from+(to-from)*static_cast<float>(index-begin)/static_cast<float>(end-begin);
        begin=end;
    }
    float previous=0;
    for(auto& stop:stops){stop.position=std::max(previous,std::min(1.0f,stop.position));previous=stop.position;}
    // Premultiplied CSS interpolation keeps the hue while fading to the
    // `transparent` keyword. Give fully transparent stops their neighbour's
    // RGB channels so Direct2D produces the same soft edge.
    for(size_t index=0;index<stops.size();++index)if((stops[index].color>>24)==0){
        const auto neighbour=index?stops[index-1].color:(index+1<stops.size()?stops[index+1].color:0u);
        stops[index].color=neighbour&0x00ffffffu;
    }
}

void PaintGradientBackgrounds(ID2D1RenderTarget* target,const ComputedStyle& style,
                              const LayoutRect& box,const CornerRadii& radius,float viewport) {
    auto background=style.Get(L"background");if(background.empty())background=style.Get(L"background-color");
    const auto layers=CommaSeparated(background);const auto rect=PixelAlignedRect(box);
    for(auto layer=layers.rbegin();layer!=layers.rend();++layer){
        const auto lowered=ToLower(Trim(*layer));
        const bool radial=lowered.find(L"radial-gradient(")!=std::wstring::npos;
        const bool linear=lowered.find(L"linear-gradient(")!=std::wstring::npos;
        if((!radial&&!linear)||lowered.empty()||lowered.back()!=L')')continue;
        const auto open=lowered.find(radial?L"radial-gradient(":L"linear-gradient(");
        const size_t prefixLength=radial?16:16;
        const auto arguments=CommaSeparated(layer->substr(open+prefixLength,layer->size()-open-prefixLength-1));
        if(arguments.size()<2)continue;
        size_t firstStop=0;
        float centerX=box.x+box.width/2.0f,centerY=box.y+box.height/2.0f;
        float angleDegrees=180.0f;
        const auto prelude=ToLower(Trim(arguments.front()));
        if(radial&&(prelude.find(L"circle")!=std::wstring::npos||prelude.find(L"ellipse")!=std::wstring::npos||prelude.find(L" at ")!=std::wstring::npos)){
            firstStop=1;const auto at=prelude.find(L" at ");
            if(at!=std::wstring::npos){const auto positions=Words(prelude.substr(at+4));
                if(!positions.empty())centerX=box.x+StyleSheet::Length(positions[0],box.width,viewport,box.width/2.0f);
                if(positions.size()>1)centerY=box.y+StyleSheet::Length(positions[1],box.height,viewport,box.height/2.0f);
            }
        }else if(linear){
            if(prelude.find(L"deg")!=std::wstring::npos){
                try{angleDegrees=std::stof(prelude);firstStop=1;}catch(...){ }
            }else if(prelude.rfind(L"to ",0)==0){
                firstStop=1;
                const bool left=prelude.find(L"left")!=std::wstring::npos;
                const bool right=prelude.find(L"right")!=std::wstring::npos;
                const bool top=prelude.find(L"top")!=std::wstring::npos;
                const bool bottom=prelude.find(L"bottom")!=std::wstring::npos;
                if((left||right)&&(top||bottom))angleDegrees=(right?(bottom?135.0f:45.0f):(bottom?225.0f:315.0f));
                else if(right)angleDegrees=90;else if(left)angleDegrees=270;
                else if(top)angleDegrees=0;else angleDegrees=180;
            }
        }
        std::vector<RadialGradientStop> parsed;
        for(size_t index=firstStop;index<arguments.size();++index){RadialGradientStop stop;if(ParseRadialGradientStop(arguments[index],stop))parsed.push_back(stop);}
        if(parsed.size()<2)continue;NormalizeGradientStops(parsed);
        std::vector<D2D1_GRADIENT_STOP> gradientStops;gradientStops.reserve(parsed.size());
        for(const auto& stop:parsed)gradientStops.push_back({stop.position,D2DColor(stop.color)});
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        if(FAILED(target->CreateGradientStopCollection(gradientStops.data(),static_cast<UINT32>(gradientStops.size()),
            D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,&collection)))continue;
        auto paint=[&](ID2D1Brush* brush){
            if(radius.x>0&&radius.y>0)target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius.x,radius.y),brush);
            else target->FillRectangle(rect,brush);
        };
        if(radial){
            const float farX=std::max(centerX-box.x,box.x+box.width-centerX);
            const float farY=std::max(centerY-box.y,box.y+box.height-centerY);
            const float circleRadius=std::max(1.0f,std::sqrt(farX*farX+farY*farY));
            Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> brush;
            const auto properties=D2D1::RadialGradientBrushProperties(D2D1::Point2F(centerX,centerY),D2D1::Point2F(0,0),circleRadius,circleRadius);
            if(SUCCEEDED(target->CreateRadialGradientBrush(properties,collection.Get(),&brush)))paint(brush.Get());
        }else{
            constexpr float pi=3.14159265358979323846f;
            const float radians=angleDegrees*pi/180.0f;
            const float directionX=std::sin(radians),directionY=-std::cos(radians);
            const float halfLength=std::max(1.0f,std::abs(directionX)*box.width/2.0f+
                std::abs(directionY)*box.height/2.0f);
            const auto start=D2D1::Point2F(centerX-directionX*halfLength,centerY-directionY*halfLength);
            const auto end=D2D1::Point2F(centerX+directionX*halfLength,centerY+directionY*halfLength);
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
            if(SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start,end),
                collection.Get(),&brush)))paint(brush.Get());
        }
    }
}

D2D1_RECT_F PixelAlignedRect(const LayoutRect& rect) {
    return D2D1::RectF(std::round(rect.x),std::round(rect.y),
        std::round(rect.x+rect.width),std::round(rect.y+rect.height));
}

void FillShadowShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,
                     const D2D1_RECT_F& base,const CornerRadii& radius,
                     const BoxShadow& shadow,float expansion) {
    const auto rect=D2D1::RectF(base.left+shadow.offsetX-expansion,
        base.top+shadow.offsetY-expansion,base.right+shadow.offsetX+expansion,
        base.bottom+shadow.offsetY+expansion);
    if(rect.right<=rect.left||rect.bottom<=rect.top)return;
    const float rx=std::max(0.0f,radius.x+expansion),ry=std::max(0.0f,radius.y+expansion);
    if(rx>0&&ry>0)target->FillRoundedRectangle(D2D1::RoundedRect(rect,rx,ry),brush);
    else target->FillRectangle(rect,brush);
}

void PaintOuterBoxShadows(ID2D1RenderTarget* target,const ComputedStyle& style,
                          const LayoutRect& rect,const CornerRadii& radius,float viewport) {
    auto shadows=BoxShadows(style,viewport);const auto base=PixelAlignedRect(rect);
    // CSS paints the first listed shadow closest to the element.
    for(auto it=shadows.rbegin();it!=shadows.rend();++it){
        const auto& shadow=*it;if(shadow.inset||(shadow.color>>24)==0)continue;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if(shadow.blur<=0.01f){
            target->CreateSolidColorBrush(D2DColor(shadow.color),&brush);
            FillShadowShape(target,brush.Get(),base,radius,shadow,shadow.spread);
            continue;
        }
        // ID2D1RenderTarget has no effect graph. Layer progressively expanded
        // silhouettes to approximate the CSS Gaussian falloff consistently.
        const int steps=std::max(2,static_cast<int>(std::ceil(shadow.blur*1.5f)));
        // Half of a Gaussian kernel lies outside the source edge. Distribute
        // that exterior opacity across the silhouettes; using the full alpha
        // here compounds every layer and makes a CSS blur roughly twice as dark.
        auto color=D2DColor(shadow.color);color.a/=steps*4.0f;
        target->CreateSolidColorBrush(color,&brush);
        for(int step=steps;step>=1;--step){
            const float expansion=shadow.spread+shadow.blur*step/steps;
            FillShadowShape(target,brush.Get(),base,radius,shadow,expansion);
        }
    }
}

void PaintInsetBoxShadows(ID2D1RenderTarget* target,const ComputedStyle& style,
                          const LayoutRect& rect,float viewport) {
    const auto shadows=BoxShadows(style,viewport);
    const auto outer=PixelAlignedRect(rect);
    if(outer.right<=outer.left||outer.bottom<=outer.top)return;
    target->PushAxisAlignedClip(outer,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for(auto it=shadows.rbegin();it!=shadows.rend();++it){
        const auto& shadow=*it;
        if(!shadow.inset||(shadow.color>>24)==0)continue;
        const int steps=shadow.blur>0.01f?std::max(2,static_cast<int>(std::ceil(shadow.blur))):1;
        auto color=D2DColor(shadow.color);
        color.a/=static_cast<float>(steps);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if(FAILED(target->CreateSolidColorBrush(color,&brush)))continue;
        auto fill=[&](float left,float top,float right,float bottom){
            if(right>left&&bottom>top)target->FillRectangle(D2D1::RectF(left,top,right,bottom),brush.Get());
        };
        for(int step=0;step<steps;++step){
            const float expansion=shadow.spread+(steps==1?0.0f:shadow.blur*step/steps);
            const float left=outer.left+shadow.offsetX+expansion;
            const float top=outer.top+shadow.offsetY+expansion;
            const float right=outer.right+shadow.offsetX-expansion;
            const float bottom=outer.bottom+shadow.offsetY-expansion;
            if(left>outer.left)fill(outer.left,outer.top,std::min(left,outer.right),outer.bottom);
            if(right<outer.right)fill(std::max(right,outer.left),outer.top,outer.right,outer.bottom);
            if(top>outer.top)fill(std::max(outer.left,left),outer.top,std::min(outer.right,right),std::min(top,outer.bottom));
            if(bottom<outer.bottom)fill(std::max(outer.left,left),std::max(bottom,outer.top),std::min(outer.right,right),outer.bottom);
        }
    }
    target->PopAxisAlignedClip();
}

std::wstring EscapeJson(const std::wstring& value){std::wstring o;for(wchar_t c:value){if(c==L'\\'||c==L'"')o+=L'\\';if(c==L'\n')o+=L"\\n";else o+=c;}return o;}

void TranslateBox(LayoutBox& box,float dx,float dy){box.rect.x+=dx;box.rect.y+=dy;box.content.x+=dx;box.content.y+=dy;for(auto& child:box.children)TranslateBox(*child,dx,dy);}
void ApplyTransform(LayoutBox& box,float viewportWidth,float viewportHeight){
    const auto transform=ToLower(Trim(box.style.Get(L"transform")));if(transform.empty()||transform==L"none")return;
    auto argument=[&](const std::wstring& function){const auto start=transform.find(function+L"(");if(start==std::wstring::npos)return std::wstring{};const auto first=start+function.size()+1,close=transform.find(L')',first);return close==std::wstring::npos?std::wstring{}:Trim(transform.substr(first,close-first));};
    float dx=0,dy=0;
    if(const auto raw=argument(L"translatex");!raw.empty())dx+=StyleSheet::Length(raw,box.rect.width,viewportWidth,0);
    if(const auto raw=argument(L"translatey");!raw.empty())dy+=StyleSheet::Length(raw,box.rect.height,viewportHeight,0);
    if(const auto raw=argument(L"translate");!raw.empty()){
        const auto values=CommaSeparated(raw);if(!values.empty())dx+=StyleSheet::Length(values[0],box.rect.width,viewportWidth,0);
        if(values.size()>1)dy+=StyleSheet::Length(values[1],box.rect.height,viewportHeight,0);
    }
    if(std::abs(dx)>0.001f||std::abs(dy)>0.001f)TranslateBox(box,dx,dy);
}

bool ApplyPaintTransform(ID2D1RenderTarget* target,const LayoutBox& box,D2D1_MATRIX_3X2_F& previous){
    const auto transform=ToLower(Trim(box.style.Get(L"transform")));
    if(transform.empty()||transform==L"none")return false;
    auto argument=[&](const std::wstring& function){const auto start=transform.find(function+L"(");if(start==std::wstring::npos)return std::wstring{};const auto first=start+function.size()+1,close=transform.find(L')',first);return close==std::wstring::npos?std::wstring{}:Trim(transform.substr(first,close-first));};
    float angle=0,scaleX=1,scaleY=1;
    if(const auto raw=argument(L"rotate");!raw.empty()){try{angle=std::stof(raw);}catch(...){}}
    if(const auto raw=argument(L"scale");!raw.empty()){
        const auto values=CommaSeparated(raw);try{if(!values.empty())scaleX=std::stof(values[0]);scaleY=values.size()>1?std::stof(values[1]):scaleX;}catch(...){scaleX=scaleY=1;}
    }
    if(const auto raw=argument(L"scalex");!raw.empty())try{scaleX=std::stof(raw);}catch(...){}
    if(const auto raw=argument(L"scaley");!raw.empty())try{scaleY=std::stof(raw);}catch(...){}
    if(std::abs(angle)<0.001f&&std::abs(scaleX-1)<0.001f&&std::abs(scaleY-1)<0.001f)return false;
    target->GetTransform(&previous);const auto center=D2D1::Point2F(box.rect.x+box.rect.width/2,box.rect.y+box.rect.height/2);
    target->SetTransform(D2D1::Matrix3x2F::Scale(scaleX,scaleY,center)*D2D1::Matrix3x2F::Rotation(angle,center)*previous);
    return true;
}
void ShiftStickyFlow(LayoutBox& box,float dy){if(!box.containsSticky)return;if(box.stickyFlowYValid)box.stickyFlowY+=dy;for(auto& child:box.children)if(child->containsSticky)ShiftStickyFlow(*child,dy);}
void ApplySticky(LayoutBox& box,float clipTop,float viewportHeight){
    if(box.style.Is(L"position",L"sticky")){if(!box.stickyFlowYValid){box.stickyFlowY=box.rect.y;box.stickyFlowYValid=true;}const float top=StyleSheet::Length(box.style.Get(L"top",L"0"),viewportHeight,viewportHeight,0),desired=std::max(box.stickyFlowY,clipTop+top);if(std::abs(box.rect.y-desired)>0.001f)TranslateBox(box,0,desired-box.rect.y);}
    for(auto& child:box.children)if(child->containsSticky)ApplySticky(*child,clipTop,viewportHeight);
    if(HasTableDisplay(box,L"table-row")||IsTableRowGroup(box)){bool found=false;float left=0,top=0,right=0,bottom=0;for(const auto& child:box.children)if(child->visible&&child->rect.width>0&&child->rect.height>0){if(!found){left=child->rect.x;top=child->rect.y;right=child->rect.x+child->rect.width;bottom=child->rect.y+child->rect.height;found=true;}else{left=std::min(left,child->rect.x);top=std::min(top,child->rect.y);right=std::max(right,child->rect.x+child->rect.width);bottom=std::max(bottom,child->rect.y+child->rect.height);}}if(found){box.rect={left,top,right-left,bottom-top};box.content=box.rect;}}
}

void ApplyScrollOffset(LayoutBox& box,float oldScrollTop,float viewportHeight){
    const float dy=oldScrollTop-box.node->scrollTop;if(std::abs(dy)<0.001f)return;
    for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed")){ShiftStickyFlow(*child,dy);TranslateBox(*child,0,dy);if(child->containsSticky)ApplySticky(*child,box.content.y,viewportHeight);}
}

std::vector<float> SvgNumbers(const std::wstring& source){
    std::vector<float> values;const wchar_t* cursor=source.c_str();
    while(*cursor){
        if(std::iswspace(*cursor)||*cursor==L','){++cursor;continue;}
        wchar_t* end=nullptr;const float value=std::wcstof(cursor,&end);
        if(end==cursor){++cursor;continue;}values.push_back(value);cursor=end;
    }
    return values;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> SvgPath(ID2D1Factory* factory,const std::wstring& data){
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if(!factory||FAILED(factory->CreatePathGeometry(&path)))return {};
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(path->Open(&sink)))return {};
    size_t position=0;wchar_t command=0,previous=0;bool open=false;
    D2D1_POINT_2F point{0,0},begin{0,0},cubic{0,0},quadratic{0,0};
    auto read=[&](float& value){
        while(position<data.size()&&(std::iswspace(data[position])||data[position]==L','))++position;
        if(position>=data.size())return false;
        wchar_t* end=nullptr;value=std::wcstof(data.c_str()+position,&end);
        if(end==data.c_str()+position)return false;
        position=static_cast<size_t>(end-data.c_str());return true;
    };
    auto nextPoint=[&](bool relative,D2D1_POINT_2F& result){
        float x=0,y=0;if(!read(x)||!read(y))return false;
        result={x+(relative?point.x:0),y+(relative?point.y:0)};return true;
    };
    while(position<data.size()){
        while(position<data.size()&&(std::iswspace(data[position])||data[position]==L','))++position;
        if(position>=data.size())break;
        if(std::iswalpha(data[position]))command=data[position++];
        if(!command){++position;continue;}
        const auto before=position;
        const bool relative=std::iswlower(command)!=0;
        const wchar_t action=static_cast<wchar_t>(std::towupper(command));
        D2D1_POINT_2F end{};
        if(action==L'Z'){
            if(open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;point=begin;}
            command=0;previous=L'Z';continue;
        }
        if(action==L'M'){
            if(!nextPoint(relative,end))break;
            if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(end,D2D1_FIGURE_BEGIN_FILLED);point=begin=end;open=true;
            command=relative?L'l':L'L';previous=L'M';continue;
        }
        if(!open){sink->BeginFigure(point,D2D1_FIGURE_BEGIN_FILLED);begin=point;open=true;}
        if(action==L'L'){
            if(!nextPoint(relative,end))break;sink->AddLine(end);point=end;
        }else if(action==L'H'||action==L'V'){
            float coordinate=0;if(!read(coordinate))break;
            end=point;
            if(action==L'H')end.x=coordinate+(relative?point.x:0);
            else end.y=coordinate+(relative?point.y:0);
            sink->AddLine(end);point=end;
        }else if(action==L'C'||action==L'S'){
            D2D1_POINT_2F first=point,second{};
            if(action==L'C'&&!nextPoint(relative,first))break;
            if(action==L'S'&&(previous==L'C'||previous==L'S'))first={2*point.x-cubic.x,2*point.y-cubic.y};
            if(!nextPoint(relative,second)||!nextPoint(relative,end))break;
            sink->AddBezier(D2D1::BezierSegment(first,second,end));point=end;cubic=second;
        }else if(action==L'Q'||action==L'T'){
            D2D1_POINT_2F control=point;
            if(action==L'Q'&&!nextPoint(relative,control))break;
            if(action==L'T'&&(previous==L'Q'||previous==L'T'))control={2*point.x-quadratic.x,2*point.y-quadratic.y};
            if(!nextPoint(relative,end))break;
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(control,end));point=end;quadratic=control;
        }else if(action==L'A'){
            float rx=0,ry=0,rotation=0,large=0,sweep=0;
            if(!read(rx)||!read(ry)||!read(rotation)||!read(large)||!read(sweep)||!nextPoint(relative,end))break;
            if(rx>0&&ry>0)sink->AddArc(D2D1::ArcSegment(end,D2D1::SizeF(rx,ry),rotation,
                sweep?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                large?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));
            else sink->AddLine(end);
            point=end;
        }else {command=0;continue;}
        previous=action;
        if(position==before)++position;
    }
    if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if(FAILED(sink->Close()))return {};
    return path;
}

std::shared_ptr<Node> FindSvgReference(const std::shared_ptr<Node>& source,const std::wstring& id){
    if(!source||id.empty())return {};
    auto root=source;while(auto parent=root->parent.lock())root=parent;
    std::shared_ptr<Node> found;
    std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& node){
        if(!node||found)return;if(node->Attribute(L"id")==id){found=node;return;}
        for(const auto& child:node->children)visit(child);
    };
    visit(root);return found;
}

std::shared_ptr<Node> SvgUseTarget(const std::shared_ptr<Node>& use){
    if(!use||use->tag!=L"use")return {};
    auto reference=use->Attribute(L"href");if(reference.empty())reference=use->Attribute(L"xlink:href");
    return !reference.empty()&&reference.front()==L'#'?FindSvgReference(use,reference.substr(1)):std::shared_ptr<Node>{};
}

void PaintSvgShape(ID2D1RenderTarget* target,ID2D1Factory* factory,StyleSheet& styleSheet,
                   const std::shared_ptr<Node>& node,const ComputedStyle* parentStyle,
                   float parentOpacity,unsigned int inheritedColor,int referenceDepth=0){
    if(!node||referenceDepth>16)return;
    const auto style=styleSheet.Compute(node,parentStyle);
    const auto currentColor=StyleSheet::Color(style.Get(L"color"),inheritedColor);
    const auto fill=style.Get(L"fill",L"black");
    const auto stroke=style.Get(L"stroke",L"none");
    const auto strokeWidthText=style.Get(L"stroke-width",L"1");
    const auto strokeWidth=StyleSheet::Length(strokeWidthText,24,24,1);
    float opacity=parentOpacity;
    try{opacity*=std::stof(style.Get(L"opacity",L"1"));}catch(...){ }
    opacity=std::max(0.0f,std::min(1.0f,opacity));
    if(node->tag==L"g"||node->tag==L"symbol"){
        for(const auto& child:node->children)
            PaintSvgShape(target,factory,styleSheet,child,&style,opacity,currentColor,referenceDepth);
        return;
    }
    if(node->tag==L"use"){
        if(const auto referenced=SvgUseTarget(node)){
            if(referenced->tag==L"symbol"||referenced->tag==L"g")
                for(const auto& child:referenced->children)
                    PaintSvgShape(target,factory,styleSheet,child,&style,opacity,currentColor,referenceDepth+1);
            else PaintSvgShape(target,factory,styleSheet,referenced,&style,opacity,currentColor,referenceDepth+1);
        }
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fillBrush,strokeBrush;
    auto color=[&](const std::wstring& raw){return raw==L"currentColor"?currentColor:StyleSheet::Color(raw,currentColor);};
    auto brushColor=[&](const std::wstring& raw,const wchar_t* opacityProperty){
        auto result=D2DColor(color(raw));float localOpacity=1;
        try{localOpacity=std::stof(style.Get(opacityProperty,L"1"));}catch(...){ }
        result.a*=opacity*std::max(0.0f,std::min(1.0f,localOpacity));return result;
    };
    if(!fill.empty()&&fill!=L"none")target->CreateSolidColorBrush(brushColor(fill,L"fill-opacity"),&fillBrush);
    if(!stroke.empty()&&stroke!=L"none")target->CreateSolidColorBrush(brushColor(stroke,L"stroke-opacity"),&strokeBrush);
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
    D2D1_STROKE_STYLE_PROPERTIES strokeProperties=D2D1::StrokeStyleProperties();
    const auto lineCap=ToLower(style.Get(L"stroke-linecap",L"butt"));
    if(lineCap==L"round")strokeProperties.startCap=strokeProperties.endCap=strokeProperties.dashCap=D2D1_CAP_STYLE_ROUND;
    else if(lineCap==L"square")strokeProperties.startCap=strokeProperties.endCap=strokeProperties.dashCap=D2D1_CAP_STYLE_SQUARE;
    const auto lineJoin=ToLower(style.Get(L"stroke-linejoin",L"miter"));
    if(lineJoin==L"round")strokeProperties.lineJoin=D2D1_LINE_JOIN_ROUND;
    else if(lineJoin==L"bevel")strokeProperties.lineJoin=D2D1_LINE_JOIN_BEVEL;
    factory->CreateStrokeStyle(strokeProperties,nullptr,0,&strokeStyle);
    const auto numbers=[&](const wchar_t* name){return SvgNumbers(node->Attribute(name));};
    auto paintGeometry=[&](ID2D1Geometry* geometry){
        if(!geometry)return;
        if(fillBrush)target->FillGeometry(geometry,fillBrush.Get());
        if(strokeBrush)target->DrawGeometry(geometry,strokeBrush.Get(),strokeWidth,strokeStyle.Get());
    };
    if(node->tag==L"path"){
        auto geometry=SvgPath(factory,node->Attribute(L"d"));
        paintGeometry(geometry.Get());
    }else if(node->tag==L"polygon"||node->tag==L"polyline"){
        const auto points=numbers(L"points");
        if(points.size()>=4){
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(factory->CreatePathGeometry(&geometry))&&
               SUCCEEDED(geometry->Open(&sink))){
                sink->BeginFigure(D2D1::Point2F(points[0],points[1]),
                    fillBrush?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);
                for(size_t index=2;index+1<points.size();index+=2)
                    sink->AddLine(D2D1::Point2F(points[index],points[index+1]));
                sink->EndFigure(node->tag==L"polygon"?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
                if(SUCCEEDED(sink->Close()))paintGeometry(geometry.Get());
            }
        }
    }else if(node->tag==L"rect"){
        const float x=StyleSheet::Length(node->Attribute(L"x"),24,24,0),y=StyleSheet::Length(node->Attribute(L"y"),24,24,0);
        const float width=StyleSheet::Length(node->Attribute(L"width"),24,24,0),height=StyleSheet::Length(node->Attribute(L"height"),24,24,0);
        const float radius=StyleSheet::Length(node->Attribute(L"rx"),24,24,0);
        auto rect=D2D1::RoundedRect(D2D1::RectF(x,y,x+width,y+height),radius,radius);
        if(fillBrush)target->FillRoundedRectangle(rect,fillBrush.Get());
        if(strokeBrush)target->DrawRoundedRectangle(rect,strokeBrush.Get(),strokeWidth,strokeStyle.Get());
    }else if(node->tag==L"circle"||node->tag==L"ellipse"){
        const float cx=StyleSheet::Length(node->Attribute(L"cx"),24,24,0),cy=StyleSheet::Length(node->Attribute(L"cy"),24,24,0);
        const float rx=StyleSheet::Length(node->Attribute(node->tag==L"circle"?L"r":L"rx"),24,24,0);
        const float ry=node->tag==L"circle"?rx:StyleSheet::Length(node->Attribute(L"ry"),24,24,0);
        const auto ellipse=D2D1::Ellipse(D2D1::Point2F(cx,cy),rx,ry);
        if(fillBrush)target->FillEllipse(ellipse,fillBrush.Get());
        if(strokeBrush)target->DrawEllipse(ellipse,strokeBrush.Get(),strokeWidth,strokeStyle.Get());
    }else if(node->tag==L"line"){
        const float x1=StyleSheet::Length(node->Attribute(L"x1"),24,24,0),y1=StyleSheet::Length(node->Attribute(L"y1"),24,24,0);
        const float x2=StyleSheet::Length(node->Attribute(L"x2"),24,24,0),y2=StyleSheet::Length(node->Attribute(L"y2"),24,24,0);
        if(strokeBrush)target->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),strokeBrush.Get(),strokeWidth,strokeStyle.Get());
    }
    for(const auto& child:node->children)
        PaintSvgShape(target,factory,styleSheet,child,&style,opacity,currentColor,referenceDepth);
}

void PaintSvg(ID2D1RenderTarget* target,const LayoutBox& box,StyleSheet& styleSheet){
    auto viewport=SvgNumbers(box.node->Attribute(L"viewbox"));
    if(viewport.empty())for(const auto& child:box.node->children)if(child->tag==L"use"){
        if(const auto referenced=SvgUseTarget(child))viewport=SvgNumbers(referenced->Attribute(L"viewbox"));
        if(!viewport.empty())break;
    }
    const float left=viewport.size()==4?viewport[0]:0,top=viewport.size()==4?viewport[1]:0;
    const float width=viewport.size()==4?viewport[2]:box.content.width;
    const float height=viewport.size()==4?viewport[3]:box.content.height;
    if(width<=0||height<=0||box.content.width<=0||box.content.height<=0)return;
    const float scale=std::min(box.content.width/width,box.content.height/height);
    const float x=box.content.x+(box.content.width-width*scale)/2;
    const float y=box.content.y+(box.content.height-height*scale)/2;
    D2D1_MATRIX_3X2_F old{};target->GetTransform(&old);
    target->SetTransform(D2D1::Matrix3x2F::Translation(-left,-top)*
        D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(x,y)*old);
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);
    const auto currentColor=StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000);
    for(const auto& child:box.node->children)
        PaintSvgShape(target,factory.Get(),styleSheet,child,&box.style,1,currentColor);
    target->SetTransform(old);
}

std::wstring GeneratedContentText(const std::wstring& source,
                                  const std::shared_ptr<Node>& origin){
    std::wstring result;size_t position=0;
    while(position<source.size()){
        while(position<source.size()&&std::iswspace(source[position]))++position;
        if(position>=source.size())break;
        const auto quote=source[position];
        if(quote==L'\''||quote==L'"'){
            ++position;
            while(position<source.size()&&source[position]!=quote){
                if(source[position]==L'\\'&&position+1<source.size())++position;
                result+=source[position++];
            }
            if(position<source.size())++position;
            continue;
        }
        const auto remaining=ToLower(source.substr(position));
        if(remaining.rfind(L"attr(",0)==0){
            const auto close=source.find(L')',position+5);
            if(close==std::wstring::npos)break;
            auto name=Trim(source.substr(position+5,close-position-5));
            const auto separator=name.find_first_of(L" ,\t\r\n");
            if(separator!=std::wstring::npos)name=name.substr(0,separator);
            if(origin&&!name.empty())result+=origin->Attribute(ToLower(name));
            position=close+1;continue;
        }
        auto end=position;
        while(end<source.size()&&!std::iswspace(source[end]))++end;
        result+=source.substr(position,end-position);position=end;
    }
    return DecodeEntities(result);
}

} // namespace

LayoutEngine::LayoutEngine(Document& document,StyleSheet& styleSheet):document_(document),styleSheet_(styleSheet){}

std::unique_ptr<LayoutBox> LayoutEngine::Build(const std::shared_ptr<Node>& node,const ComputedStyle* parentStyle,std::uint64_t parentContext,size_t siblingIndex,size_t siblingCount,const std::shared_ptr<Node>& previousElement){
    if(!node)return {};auto box=std::make_unique<LayoutBox>();box->node=node;boxIndex_[node.get()]=box.get();const auto cacheKey=StyleContextHash(node,parentContext,styleSheet_,siblingIndex,siblingCount,previousElement);auto cached=styleCache_.find(cacheKey);if(cached!=styleCache_.end())box->style=cached->second;else{box->style=styleSheet_.Compute(node,parentStyle);styleCache_.emplace(cacheKey,box->style);}box->style.deviceScale=deviceScale_;box->visible=!box->style.Is(L"display",L"none");
    if(!box->visible)return box;
    auto appendPseudo=[&](const wchar_t* name){
        if(!styleSheet_.HasPseudoRulesFor(node,name))return;
        auto pseudoStyle=styleSheet_.Compute(node,&box->style,name);pseudoStyle.deviceScale=deviceScale_;const auto content=Trim(pseudoStyle.Get(L"content"));
        if(content.empty()||content==L"none"||content==L"normal")return;
        auto generated=std::make_shared<Node>();generated->tag=L"span";generated->parent=node;
        auto pseudoBox=std::make_unique<LayoutBox>();pseudoBox->node=generated;pseudoBox->generatedFrom=node;
        pseudoBox->pseudo=name;pseudoBox->style=std::move(pseudoStyle);pseudoBox->visible=!pseudoBox->style.Is(L"display",L"none");
        const auto value=GeneratedContentText(content,node);
        if(pseudoBox->visible&&!value.empty()){
            auto text=std::make_shared<Node>();text->type=NodeType::Text;text->tag=L"#text";text->text=value;text->parent=generated;
            generated->children.push_back(text);auto textBox=Build(text,&pseudoBox->style,parentContext^HashText(name));
            if(textBox){textBox->parent=pseudoBox.get();pseudoBox->children.push_back(std::move(textBox));}
        }
        pseudoBox->parent=box.get();box->children.push_back(std::move(pseudoBox));
    };
    appendPseudo(L"before");
    const auto parentDisplay=box->style.Get(L"display");
    const bool anonymousLayoutItem=parentDisplay==L"flex"||parentDisplay==L"inline-flex"||
        parentDisplay==L"grid"||parentDisplay==L"inline-grid";
    bool hasInlineContent=false;
    size_t elementCount=0;for(const auto& child:node->children)if(child->type==NodeType::Element)++elementCount;
    const auto parentWhiteSpace=box->style.Get(L"white-space");
    const auto hasRenderableText=[&](const std::wstring& text){
        // Flex and grid turn direct text runs into anonymous layout items, but
        // a run containing only document-formatting whitespace does not
        // generate an item.  This remains true when white-space: pre is used
        // on the container (for example, to preserve shortcut-label tabs).
        if(anonymousLayoutItem&&std::all_of(text.begin(),text.end(),
            [](wchar_t character){return std::iswspace(character)!=0;}))return false;
        const auto normalized=NormalizeText(text,parentWhiteSpace);
        return PreservesLineBreaks(parentWhiteSpace)?!normalized.empty():!Trim(normalized).empty();
    };
    std::vector<bool> hasFollowingContent(node->children.size());bool following=false;
    for(size_t index=node->children.size();index>0;--index){const auto& child=node->children[index-1];hasFollowingContent[index-1]=following;if(child->type==NodeType::Text){if(hasRenderableText(child->text))following=true;}else following=true;}
    const auto appendBuilt=[&](std::unique_ptr<LayoutBox> built,
                               const std::shared_ptr<Node>& sourceNode,
                               bool preserveLeading,bool preserveTrailing){
        if(!built)return;
        built->parent=box.get();
        if(sourceNode&&sourceNode->type==NodeType::Text&&built->node!=sourceNode){
            built->generatedFrom=sourceNode;
            if(!boxIndex_.count(sourceNode.get()))boxIndex_[sourceNode.get()]=built.get();
        }
        const bool inlineLevel=built->node->type==NodeType::Text||IsInlineLevel(built->style.Get(L"display"));
        if(built->node->type==NodeType::Text&&!anonymousLayoutItem){
            built->preserveLeadingWhitespace=preserveLeading;
            built->preserveTrailingWhitespace=preserveTrailing;
        }
        hasInlineContent=inlineLevel;box->containsSticky=box->containsSticky||built->containsSticky;
        box->children.push_back(std::move(built));
    };
    size_t elementIndex=0;std::shared_ptr<Node> previousChildElement;
    for(size_t childIndex=0;childIndex<node->children.size();++childIndex){auto& child=node->children[childIndex];
        if(node->tag==L"svg")break; // SVG descendants are painted in the SVG viewport, not HTML flow.
        if(child->type==NodeType::Text&&!hasRenderableText(child->text))continue;
        if(child->type==NodeType::Text&&PreservesLineBreaks(parentWhiteSpace)){
            const auto normalized=NormalizeText(child->text,parentWhiteSpace);
            if(normalized.find(L'\n')!=std::wstring::npos){
                size_t start=0;
                while(start<=normalized.size()){
                    const auto end=normalized.find(L'\n',start);
                    const auto length=(end==std::wstring::npos?normalized.size():end)-start;
                    if(length){
                        auto fragment=std::make_shared<Node>();fragment->type=NodeType::Text;
                        fragment->tag=L"#text";fragment->text=normalized.substr(start,length);fragment->parent=node;
                        auto fragmentBox=Build(fragment,&box->style,cacheKey);
                        if(fragmentBox)fragmentBox->textSourceOffset=start;
                        appendBuilt(std::move(fragmentBox),child,hasInlineContent,
                            end==std::wstring::npos&&hasFollowingContent[childIndex]);
                    }
                    if(end==std::wstring::npos)break;
                    auto lineBreak=std::make_shared<Node>();lineBreak->tag=L"br";lineBreak->parent=node;
                    appendBuilt(Build(lineBreak,&box->style,cacheKey),{},false,false);
                    hasInlineContent=false;start=end+1;
                }
                continue;
            }
        }
        if(child->type==NodeType::Element)++elementIndex;
        auto built=Build(child,&box->style,cacheKey,child->type==NodeType::Element?elementIndex:0,elementCount,previousChildElement);if(child->type==NodeType::Element)previousChildElement=child;
        appendBuilt(std::move(built),child,hasInlineContent,hasFollowingContent[childIndex]);
    }
    appendPseudo(L"after");
    box->containsSticky=box->containsSticky||box->style.Is(L"position",L"sticky");
    return box;
}

void LayoutEngine::ApplyTransitions(LayoutBox& box){
    if(!box.generatedFrom&&box.node){
        const auto key=box.node.get();
        const auto definitions=TransitionDefinitions(box.style);
        const auto previous=transitionTargets_.find(key);
        auto active=transitions_.find(key);
        if(previous!=transitionTargets_.end()){
            std::vector<std::wstring> properties;
            bool includesAll=false;
            for(const auto& definition:definitions){
                if(definition.property==L"all")includesAll=true;
                else if(std::find(properties.begin(),properties.end(),definition.property)==properties.end())
                    properties.push_back(definition.property);
            }
            if(includesAll){
                for(const auto& item:*previous->second.values)
                    if(std::find(properties.begin(),properties.end(),item.first)==properties.end())properties.push_back(item.first);
                for(const auto& item:*box.style.values)
                    if(std::find(properties.begin(),properties.end(),item.first)==properties.end())properties.push_back(item.first);
            }
            for(const auto& property:properties){
                if(property.empty()||property.rfind(L"transition",0)==0||property.rfind(L"--",0)==0)continue;
                auto oldValue=TransitionBaseValue(previous->second,property,box.style.Get(property));
                auto newValue=TransitionBaseValue(box.style,property,oldValue);
                if(property==L"transform"){
                    if((oldValue.empty()||ToLower(oldValue)==L"none")&&newValue!=L"none")oldValue=IdentityTransform(newValue);
                    if((newValue.empty()||ToLower(newValue)==L"none")&&oldValue!=L"none")newValue=IdentityTransform(oldValue);
                }
                if(oldValue==newValue)continue;
                std::wstring currentValue=oldValue;
                if(active!=transitions_.end()){
                    for(const auto& transition:active->second)
                        if(transition.property==property){currentValue=TransitionValue(transition);break;}
                    active->second.erase(std::remove_if(active->second.begin(),active->second.end(),
                        [&](const StyleTransition& transition){return transition.property==property;}),active->second.end());
                }
                if(currentValue==newValue)continue;
                const auto* definition=TransitionFor(definitions,property);
                const bool discrete=property==L"visibility";
                if(!definition||(definition->durationMs<=0&&definition->delayMs<=0)||
                   (!discrete&&(!IsNumericTransitionProperty(property)||!CanInterpolateNumbers(currentValue,newValue))))continue;
                StyleTransition transition;
                transition.property=property;transition.from=std::move(currentValue);transition.to=std::move(newValue);
                transition.durationMs=definition->durationMs;transition.delayMs=definition->delayMs;
                transition.x1=definition->x1;transition.y1=definition->y1;
                transition.x2=definition->x2;transition.y2=definition->y2;transition.discrete=discrete;
                if(active==transitions_.end())active=transitions_.emplace(key,std::vector<StyleTransition>{}).first;
                active->second.push_back(std::move(transition));
            }
        }
        if(!definitions.empty()||active!=transitions_.end())transitionTargets_[key]=box.style;
        if(active!=transitions_.end()){
            active->second.erase(std::remove_if(active->second.begin(),active->second.end(),TransitionComplete),active->second.end());
            if(!active->second.empty()){
                box.style.values=std::make_shared<ComputedStyle::ValueMap>(*box.style.values);
                for(const auto& transition:active->second)(*box.style.values)[transition.property]=TransitionValue(transition);
            }else transitions_.erase(key);
        }
    }
    for(auto& child:box.children)ApplyTransitions(*child);
}

bool LayoutEngine::HasActiveTransitions()const{
    for(const auto& item:transitions_)if(!item.second.empty())return true;
    return false;
}

void LayoutEngine::RefreshTransitionFrame(LayoutBox& box){
    if(!box.generatedFrom&&box.node){
        const auto key=box.node.get();auto active=transitions_.find(key);
        if(active!=transitions_.end()){
            const auto target=transitionTargets_.find(key);
            if(target!=transitionTargets_.end())box.style=target->second;
            const bool intrinsicChanged=std::any_of(active->second.begin(),active->second.end(),[](const StyleTransition& transition){
                return transition.property!=L"opacity"&&transition.property!=L"visibility"&&
                       transition.property!=L"transform"&&transition.property!=L"z-index";
            });
            active->second.erase(std::remove_if(active->second.begin(),active->second.end(),TransitionComplete),active->second.end());
            if(active->second.empty())transitions_.erase(key);
            else{
                box.style.values=std::make_shared<ComputedStyle::ValueMap>(*box.style.values);
                for(const auto& transition:active->second)(*box.style.values)[transition.property]=TransitionValue(transition);
            }
            if(intrinsicChanged)for(auto* current=&box;current;current=current->parent){
                current->naturalWidthValid=false;current->minimumWidthValid=false;current->naturalHeightValid=false;
            }
        }
    }
    for(auto& child:box.children)RefreshTransitionFrame(*child);
}

bool LayoutEngine::AdvanceTransitions(float milliseconds){
    milliseconds=std::max(0.0f,milliseconds);
    for(auto& item:transitions_)for(auto& transition:item.second){
        transition.elapsedMs+=milliseconds;
    }
    if(root_){
        RefreshTransitionFrame(*root_);
        root_->rect={0,0,viewportWidth_,viewportHeight_};root_->content=root_->rect;
        LayoutBoxTree(*root_,root_->rect,true);
        UpdateTraversalMetadata(*root_);
        UpdateStackingContexts(*root_);
    }
    return HasActiveTransitions();
}

void LayoutEngine::ClearTransitions(){
    transitionTargets_.clear();transitions_.clear();
}

void LayoutEngine::Layout(float width,float height,float deviceScale){
    deviceScale_=std::max(0.01f,deviceScale);
    viewportWidth_=std::max(1.0f,width);viewportHeight_=std::max(1.0f,height);
    styleSheet_.SetViewport(viewportWidth_,viewportHeight_);
    if(styleCacheVersion_!=styleSheet_.Version()||styleCache_.size()>8192){
        styleCache_.clear();styleCacheVersion_=styleSheet_.Version();
    }
    boxIndex_.clear();auto rootNode=document_.Body();if(!rootNode)rootNode=document_.Root();
    std::uint64_t context=1469598103934665603ull;
    std::vector<std::shared_ptr<Node>> ancestors;
    for(auto ancestor=rootNode->parent.lock();ancestor;ancestor=ancestor->parent.lock())ancestors.push_back(ancestor);
    std::vector<ComputedStyle> ancestorStyles;ancestorStyles.reserve(ancestors.size());
    const ComputedStyle* inheritedStyle=nullptr;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        context=StyleContextHash(*it,context,styleSheet_);
        ancestorStyles.push_back(styleSheet_.Compute(*it,inheritedStyle));
        inheritedStyle=&ancestorStyles.back();
    }
    root_=Build(rootNode,inheritedStyle,context);if(!root_)return;
    ApplyTransitions(*root_);
    std::vector<const Node*> removed;
    for(const auto& item:transitionTargets_)if(!boxIndex_.count(item.first))removed.push_back(item.first);
    for(const auto key:removed){transitionTargets_.erase(key);transitions_.erase(key);}
    root_->rect={0,0,viewportWidth_,viewportHeight_};root_->content=root_->rect;LayoutBoxTree(*root_,root_->rect,true);
    UpdateTraversalMetadata(*root_);
    UpdateStackingContexts(*root_);
}

void LayoutEngine::InvalidateMeasurements(LayoutBox& box){
    box.naturalWidthValid=false;box.minimumWidthValid=false;box.naturalHeightValid=false;
    box.stickyFlowYValid=false;
    for(auto& child:box.children)InvalidateMeasurements(*child);
}

void LayoutEngine::Relayout(float width,float height,float deviceScale){
    width=std::max(1.0f,width);height=std::max(1.0f,height);
    deviceScale=std::max(0.01f,deviceScale);
    const auto previousStyleVersion=styleSheet_.Version();
    styleSheet_.SetViewport(width,height);
    if(!root_||styleSheet_.Version()!=previousStyleVersion||std::abs(deviceScale-deviceScale_)>0.0001f){
        Layout(width,height,deviceScale);
        return;
    }
    viewportWidth_=width;viewportHeight_=height;
    InvalidateMeasurements(*root_);
    root_->rect={0,0,viewportWidth_,viewportHeight_};root_->content=root_->rect;
    LayoutBoxTree(*root_,root_->rect,true);
    UpdateTraversalMetadata(*root_);
    UpdateStackingContexts(*root_);
}

void LayoutEngine::LayoutBoxTree(LayoutBox& box,const LayoutRect& available,bool forcedSize,
                                 bool definiteWidth,bool definiteHeight){
    if(!box.visible)return;
    if(box.node->type==NodeType::Text){
        // Text nodes do not generate an independently stylable CSS box. Their
        // inline/block parent has already resolved the content rectangle, so
        // running element margin, border, overflow and transform resolution is
        // both redundant and observably expensive in large code/list views.
        box.rect=available;
        box.content=available;
        return;
    }
    auto margin=EdgeValues(box.style,L"margin",available.width,viewportWidth_);auto padding=EdgeValues(box.style,L"padding",available.width,viewportWidth_);
    const auto border=BorderValues(box.style);float x=available.x+margin.left,y=available.y+margin.top;
    float width=std::max(0.0f,available.width-margin.left-margin.right),height=std::max(0.0f,available.height-margin.top-margin.bottom);
    auto cssWidth=box.style.Get(L"width"),cssHeight=box.style.Get(L"height");
    if(!forcedSize&& !cssWidth.empty()&&cssWidth!=L"auto")width=StyleSheet::Length(cssWidth,available.width,viewportWidth_,width);
    if(!forcedSize&& !cssHeight.empty()&&cssHeight!=L"auto")height=StyleSheet::Length(cssHeight,available.height,viewportHeight_,height);
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");if(!borderBox&&!forcedSize){if(!cssWidth.empty()&&cssWidth!=L"auto")width+=padding.left+padding.right+border.left+border.right;if(!cssHeight.empty()&&cssHeight!=L"auto")height+=padding.top+padding.bottom+border.top+border.bottom;}
    box.rect={x,y,std::max(0.0f,width),std::max(0.0f,height)};box.content={x+border.left+padding.left,y+border.top+padding.top,std::max(0.0f,width-border.left-border.right-padding.left-padding.right),std::max(0.0f,height-border.top-border.bottom-padding.top-padding.bottom)};
    const auto display=box.style.Get(L"display");if(display==L"flex"||display==L"inline-flex")LayoutFlex(box);else if(display==L"grid"||display==L"inline-grid")LayoutGrid(box,definiteWidth,definiteHeight);else if(display==L"table")LayoutTable(box);else LayoutBlock(box,definiteHeight);FinalizeScroll(box);ApplyTransform(box,viewportWidth_,viewportHeight_);
}

void LayoutEngine::UpdateTraversalMetadata(LayoutBox& box){
    for(auto& child:box.children)UpdateTraversalMetadata(*child);

    box.paintChildren.clear();
    box.paintChildren.reserve(box.children.size());
    for(auto& child:box.children)box.paintChildren.push_back(child.get());
    StableStackingOrder(box.paintChildren);

    // Any sufficiently large, non-overlapping vertical flow can skip directly
    // to the visible children. This is based only on final layout geometry and
    // CSS positioning, so it applies equally to lists, code views, tables, and
    // application-defined components.
    box.verticallyOrderedChildren.clear();
    box.overlayChildren.clear();
    box.verticallyOrderedChildren.reserve(box.paintChildren.size());
    float previousBottom=-std::numeric_limits<float>::infinity();
    for(auto* child:box.paintChildren){
        const auto position=child->style.Get(L"position",L"static");
        if(!child->visible||child->rect.width<=0||child->rect.height<=0)continue;
        if(child->containsSticky||position==L"absolute"||position==L"fixed"||
           position==L"sticky"){
            box.overlayChildren.push_back(child);
            continue;
        }
        const float bottom=child->rect.y+child->rect.height;
        if(child->rect.y<previousBottom-0.01f){
            box.verticallyOrderedChildren.clear();
            box.overlayChildren.clear();
            break;
        }
        box.verticallyOrderedChildren.push_back(child);
        previousBottom=bottom;
    }
    if(box.verticallyOrderedChildren.size()<8){
        box.verticallyOrderedChildren.clear();
        box.overlayChildren.clear();
    }
}

void LayoutEngine::UpdateStackingContexts(LayoutBox& scope){
    scope.nonNegativeStackingContexts.clear();
    std::function<void(LayoutBox&)> collect=[&](LayoutBox& current){
        for(auto& child:current.children){
            if(IsStackingContext(*child)){
                // CSS paints zero/auto positioned stacking contexts after
                // ordinary in-flow descendants. Sticky boxes always establish
                // a stacking context, even without an explicit z-index.
                if(ZIndex(*child)>=0)scope.nonNegativeStackingContexts.push_back(child.get());
                UpdateStackingContexts(*child);
            }else collect(*child);
        }
    };
    collect(scope);
    StableStackingOrder(scope.nonNegativeStackingContexts);
}

void LayoutEngine::FinalizeScroll(LayoutBox& box){
    const auto overflowY=box.style.Get(L"overflow-y",box.style.Get(L"overflow",L"visible"));
    box.scrollHeight=box.content.height;
    if(overflowY!=L"auto"&&overflowY!=L"scroll")return;
    if(box.node->tag==L"textarea"){
        const auto padding=EdgeValues(box.style,L"padding",box.rect.width,viewportWidth_);
        box.scrollHeight=std::max(box.content.height,
            TextHeight(box.node->Attribute(L"value"),box.style,box.content.width)+padding.bottom);
        const float maximum=std::max(0.0f,box.scrollHeight-box.content.height);
        box.node->scrollTop=std::max(0.0f,std::min(maximum,box.node->scrollTop));
        return;
    }
    std::function<float(const LayoutBox&)> measure=[&](const LayoutBox& current){
        if(!current.visible||current.style.Is(L"position",L"fixed"))return current.content.y;
        float descendantBottom=current.content.y;
        for(const auto& child:current.children)descendantBottom=std::max(descendantBottom,measure(*child));
        float extent=std::max(current.rect.y+current.rect.height,descendantBottom);
        const float contentBottom=current.content.y+current.content.height;
        if(descendantBottom>contentBottom+0.01f){
            // End padding follows overflowing content in the scrollable
            // overflow area. A forced grid/flex item may be shorter than its
            // contents, but its authored padding must not disappear.
            const float endInset=std::max(0.0f,current.rect.y+current.rect.height-contentBottom);
            extent=std::max(extent,descendantBottom+endInset);
        }
        return extent;
    };
    float bottom=box.content.y;
    for(const auto& child:box.children)bottom=std::max(bottom,measure(*child));
    box.scrollHeight=std::max(box.content.height,bottom-box.content.y);
    const float maximum=std::max(0.0f,box.scrollHeight-box.content.height);
    box.node->scrollTop=std::max(0.0f,std::min(maximum,box.node->scrollTop));
    if(box.node->scrollTop>0){
        for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed"))TranslateBox(*child,0,-box.node->scrollTop);
        for(auto& child:box.children)ApplySticky(*child,box.content.y,viewportHeight_);
    }
}

void LayoutEngine::LayoutBlock(LayoutBox& box,bool definiteHeight){
    float flowWidth=box.content.width;const auto overflowY=box.style.Get(L"overflow-y",L"visible");
    if(overflowY==L"scroll"||((overflowY==L"auto")&&HasStableScrollbarGutter(box.style)))
        flowWidth=std::max(0.0f,flowWidth-VerticalScrollbarMetricsFor(box,styleSheet_).width);
    else if(overflowY==L"auto"){
        float required=0;
        for(const auto& child:box.children)
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&
               !child->style.Is(L"position",L"fixed")){
                required+=NaturalHeight(*child,flowWidth);
                // Overflow only needs a yes/no answer. Once the content has
                // crossed the viewport, measuring every remaining descendant
                // repeats the same intrinsic grid/text work that normal flow
                // performs immediately below.
                if(required>box.content.height+1)break;
            }
        if(required>box.content.height+1)
            flowWidth=std::max(0.0f,flowWidth-VerticalScrollbarMetricsFor(box,styleSheet_).width);
    }
    // A single no-wrap text run clipped by its block does not participate in
    // sibling flow. Give it the block's content rectangle directly so text
    // alignment, clipping, and ellipsis use the CSS containing block without
    // an otherwise redundant DirectWrite natural-width measurement.
    if(auto* textChild=SingleLineClippedTextChild(box)){
        LayoutBoxTree(*textChild,{box.content.x,box.content.y,flowWidth,box.content.height},true);
        return;
    }
    auto inlineOuterWidth=[&](const LayoutBox& child){
        const auto width=child.style.Get(L"width");
        return !width.empty()&&width!=L"auto"?BlockOuterWidth(child,flowWidth,viewportWidth_):NaturalWidth(child);
    };
    auto inlineOuterHeight=[&](const LayoutBox& child,float width){
        const auto height=child.style.Get(L"height");
        return !height.empty()&&height!=L"auto"?
            BlockOuterHeight(child,box.content.height,width,viewportHeight_,viewportWidth_):
            NaturalHeight(child,width);
    };
    bool inlineOnly=!box.children.empty();std::vector<std::pair<float,float>> inlineSizes;float inlineWidth=0,inlineHeight=0;
    for(auto& child:box.children){if(!child->visible)continue;if(child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;if(child->node->tag==L"br"){inlineOnly=false;break;}const auto display=child->style.Get(L"display");if(!IsInlineLevel(display)){inlineOnly=false;break;}const float width=inlineOuterWidth(*child),height=inlineOuterHeight(*child,width);inlineSizes.push_back({width,height});inlineWidth+=width;inlineHeight=std::max(inlineHeight,height);}
    const auto whiteSpace=box.style.Get(L"white-space");
    const bool noWrap=PreventsTextWrapping(whiteSpace);
    if(inlineOnly&&(!inlineSizes.empty())&&(inlineWidth<=flowWidth+0.5f||noWrap)){
        float x=box.content.x;if(box.style.Is(L"text-align",L"center"))x+=(flowWidth-inlineWidth)/2;else if(box.style.Is(L"text-align",L"right"))x+=flowWidth-inlineWidth;
        float y=box.content.y;
        // Ordinary inline formatting starts at the block's content edge.
        // A table cell, however, distributes the row's extra block size around
        // its inline line.  Keep a mixed line (for example icon + text) on the
        // same vertical center as the single text-run fast path used by sibling
        // cells.  Work in CSS DIPs so the result is stable at every monitor DPI.
        const bool tableCell=box.style.Is(L"display",L"table-cell");
        const auto verticalAlign=ToLower(Trim(box.style.Get(L"vertical-align")));
        const float verticalFree=std::max(0.0f,box.content.height-inlineHeight);
        if(box.node&&box.node->tag==L"button")y+=verticalFree/2;
        else if(tableCell){
            if(verticalAlign==L"bottom"||verticalAlign==L"text-bottom")y+=verticalFree;
            else if(verticalAlign!=L"top"&&verticalAlign!=L"text-top")y+=verticalFree/2;
        }
        size_t index=0;
        for(auto& child:box.children)if(child->visible){
            if(child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed")){
                const LayoutRect area=child->style.Is(L"position",L"fixed")?LayoutRect{0,0,viewportWidth_,viewportHeight_}:AbsoluteContainingBlock(box);
                LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);continue;
            }
            const auto size=inlineSizes[index++];LayoutBoxTree(*child,{x,y,size.first,size.second},true);x+=size.first;
        }
        return;
    }
    float cursorY=box.content.y;float lineX=box.content.x;float lineHeight=0;
    for(auto& child:box.children){if(!child->visible)continue;
        const bool absolute=child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed");
        if(absolute){
            const LayoutRect area=child->style.Is(L"position",L"fixed")?LayoutRect{0,0,viewportWidth_,viewportHeight_}:AbsoluteContainingBlock(box);
            LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);continue;
        }
        if(child->node->tag==L"br"){
            const float breakHeight=LineHeight(child->style);
            LayoutBoxTree(*child,{lineX,cursorY,0,breakHeight},true,false,false);
            cursorY+=lineX>box.content.x?std::max(lineHeight,breakHeight):breakHeight;
            lineX=box.content.x;lineHeight=0;continue;
        }
        const auto d=child->style.Get(L"display");const bool inlineBox=IsInlineLevel(d);
        if(inlineBox){float w=inlineOuterWidth(*child);const bool wrapText=child->node->type==NodeType::Text&&!noWrap;const bool atomic=IsAtomicInlineLevel(*child);const bool wrappingInlineContainer=!noWrap&&child->node->type==NodeType::Element&&!atomic&&!child->children.empty();if((wrapText||wrappingInlineContainer)&&lineX+w>box.content.x+flowWidth+0.5f){if(lineX>box.content.x){cursorY+=lineHeight;lineX=box.content.x;lineHeight=0;}w=std::min(w,flowWidth);}float h=(wrapText||wrappingInlineContainer)?NaturalHeight(*child,w):inlineOuterHeight(*child,w);if(!wrapText&&!wrappingInlineContainer&&lineX+w>box.content.x+flowWidth+0.5f&&lineX>box.content.x){cursorY+=lineHeight;lineX=box.content.x;lineHeight=0;}const float baselineOffset=atomic?0.0f:std::max(0.0f,TextBaselineOffset(box.style)-TextBaselineOffset(child->style));LayoutBoxTree(*child,{lineX,cursorY+baselineOffset,w,h},true,false,false);lineX+=w;lineHeight=std::max(LineHeight(box.style),std::max(lineHeight,h+(atomic?InlineFormattingDescent(box.style):0.0f)));}
        else{
            if(lineX>box.content.x){cursorY+=lineHeight;lineX=box.content.x;lineHeight=0;}
            float h=NaturalHeight(*child,flowWidth);const auto cssH=child->style.Get(L"height");
            const bool explicitHeight=!cssH.empty()&&cssH!=L"auto";
            if(explicitHeight)
                h=BlockOuterHeight(*child,box.content.height,flowWidth,viewportHeight_,viewportWidth_);
            else if(definiteHeight){
                const auto minimum=Trim(child->style.Get(L"min-height"));
                const auto maximum=Trim(child->style.Get(L"max-height"));
                const bool percentageConstraint=minimum.find(L'%')!=std::wstring::npos||
                    maximum.find(L'%')!=std::wstring::npos;
                if(percentageConstraint){
                    const auto childMargin=EdgeValues(child->style,L"margin",flowWidth,viewportWidth_);
                    const auto childPadding=EdgeValues(child->style,L"padding",flowWidth,viewportWidth_);
                    const auto childBorder=BorderValues(child->style);
                    const float decoration=child->style.Is(L"box-sizing",L"border-box")?0.0f:
                        childPadding.top+childPadding.bottom+childBorder.top+childBorder.bottom;
                    float constrained=std::max(0.0f,h-childMargin.top-childMargin.bottom-decoration);
                    constrained=Constrain(child->style,L"min-height",L"max-height",constrained,
                                          box.content.height,viewportHeight_);
                    h=constrained+decoration+childMargin.top+childMargin.bottom;
                }
            }
            const float w=BlockOuterWidth(*child,flowWidth,viewportWidth_);
            const bool autoLeft=ToLower(Trim(child->style.Get(L"margin-left")))==L"auto";
            const bool autoRight=ToLower(Trim(child->style.Get(L"margin-right")))==L"auto";
            const float freeWidth=std::max(0.0f,flowWidth-w);float childX=box.content.x;
            if(autoLeft&&autoRight)childX+=freeWidth/2;else if(autoLeft)childX+=freeWidth;
            LayoutBoxTree(*child,{childX,cursorY,w,h},true,true,explicitHeight);cursorY+=h;
        }
    }
}

void LayoutEngine::LayoutFlex(LayoutBox& box){
    std::vector<LayoutBox*> children;for(auto& c:box.children)if(c->visible&& !c->style.Is(L"position",L"absolute")&&!c->style.Is(L"position",L"fixed"))children.push_back(c.get());
    const bool column=box.style.Is(L"flex-direction",L"column");const float mainSize=column?box.content.height:box.content.width;const float crossSize=column?box.content.width:box.content.height;const float gap=GapValue(box.style,!column,mainSize,viewportWidth_);
    const auto wrapMode=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
    if(!column&&(wrapMode==L"wrap"||wrapMode==L"wrap-reverse")&&!children.empty()){
        const size_t count=children.size();
        std::vector<float> sizes(count),minimums(count),grows(count),shrinkWeights(count),crossExtents(count);
        std::vector<bool> mainAutoBefore(count),mainAutoAfter(count),
            crossAutoBefore(count),crossAutoAfter(count),crossDefinite(count);
        std::vector<std::wstring> alignments(count);
        const auto parentAlign=box.style.Get(L"align-items",L"stretch");
        for(size_t index=0;index<count;++index){
            auto* child=children[index];
            const auto margin=EdgeValues(child->style,L"margin",mainSize,viewportWidth_);
            const float mainMargin=margin.left+margin.right;
            const auto isAutoMargin=[&](const wchar_t* property){
                return ToLower(Trim(child->style.Get(property)))==L"auto";
            };
            mainAutoBefore[index]=isAutoMargin(L"margin-left");
            mainAutoAfter[index]=isAutoMargin(L"margin-right");
            crossAutoBefore[index]=isAutoMargin(L"margin-top");
            crossAutoAfter[index]=isAutoMargin(L"margin-bottom");
            grows[index]=StyleSheet::Length(child->style.Get(L"flex-grow",L"0"),0,0,0);
            const float shrink=StyleSheet::Length(child->style.Get(L"flex-shrink",L"1"),0,0,1);
            auto raw=child->style.Get(L"flex-basis");
            if(raw.empty()||raw==L"auto")raw=child->style.Get(L"width");
            const bool natural=raw.empty()||raw==L"auto"||ToLower(Trim(raw))==L"max-content";
            float base=natural?NaturalWidth(*child):StyleSheet::Length(raw,mainSize,viewportWidth_,0);
            if(!natural){
                base=Constrain(child->style,L"min-width",L"max-width",base,mainSize,viewportWidth_);
                base+=mainMargin;
            }
            sizes[index]=base;minimums[index]=MinContentWidth(*child);
            shrinkWeights[index]=shrink*std::max(0.0f,base-mainMargin);

            auto align=child->style.Get(L"align-self",L"auto");
            if(align.empty()||align==L"auto")align=parentAlign;
            alignments[index]=align;
            const auto height=child->style.Get(L"height");
            crossDefinite[index]=!height.empty()&&height!=L"auto";
            if(crossDefinite[index]){
                crossExtents[index]=StyleSheet::Length(height,crossSize,viewportHeight_,0)+
                    margin.top+margin.bottom;
            }else{
                crossExtents[index]=NaturalHeight(*child,std::max(1.0f,sizes[index]));
            }
        }

        struct FlexLine { std::vector<size_t> items; float cross=0; };
        std::vector<FlexLine> lines(1);float lineMain=0;
        for(size_t index=0;index<count;++index){
            auto& line=lines.back();
            const float candidate=lineMain+(line.items.empty()?0.0f:gap)+sizes[index];
            if(!line.items.empty()&&candidate>mainSize+0.5f){
                lines.push_back({});lineMain=0;
            }
            auto& destination=lines.back();
            if(!destination.items.empty())lineMain+=gap;
            destination.items.push_back(index);lineMain+=sizes[index];
        }

        for(auto& line:lines){
            float occupied=gap*std::max(0,static_cast<int>(line.items.size())-1);
            float growTotal=0,shrinkTotal=0;
            for(const auto index:line.items){
                occupied+=sizes[index];growTotal+=grows[index];shrinkTotal+=shrinkWeights[index];
            }
            const float freeSpace=mainSize-occupied;
            if(freeSpace>0&&growTotal>0){
                for(const auto index:line.items)if(grows[index]>0)
                    sizes[index]+=freeSpace*grows[index]/growTotal;
            }else if(freeSpace<0&&shrinkTotal>0){
                float deficit=-freeSpace;std::vector<size_t> active;
                for(const auto index:line.items)
                    if(shrinkWeights[index]>0&&sizes[index]>minimums[index]+0.01f)
                        active.push_back(index);
                while(deficit>0.01f&&!active.empty()){
                    float weightTotal=0;for(const auto index:active)weightTotal+=shrinkWeights[index];
                    if(weightTotal<=0)break;
                    std::vector<size_t> clamped;
                    for(const auto index:active){
                        const float reduction=deficit*shrinkWeights[index]/weightTotal;
                        if(sizes[index]-reduction<minimums[index])clamped.push_back(index);
                    }
                    if(clamped.empty()){
                        for(const auto index:active)
                            sizes[index]-=deficit*shrinkWeights[index]/weightTotal;
                        break;
                    }
                    for(const auto index:clamped){
                        const float reduction=std::max(0.0f,sizes[index]-minimums[index]);
                        sizes[index]=minimums[index];deficit-=reduction;
                        active.erase(std::remove(active.begin(),active.end(),index),active.end());
                    }
                }
            }
            for(const auto index:line.items){
                if(!crossDefinite[index])
                    crossExtents[index]=NaturalHeight(*children[index],std::max(1.0f,sizes[index]));
                line.cross=std::max(line.cross,crossExtents[index]);
            }
        }

        const float crossGap=GapValue(box.style,false,mainSize,viewportWidth_);
        float occupiedCross=crossGap*std::max(0,static_cast<int>(lines.size())-1);
        for(const auto& line:lines)occupiedCross+=line.cross;
        float crossRemain=std::max(0.0f,crossSize-occupiedCross);
        auto alignContent=ToLower(Trim(box.style.Get(L"align-content",L"stretch")));
        if(alignContent.empty()||alignContent==L"normal")alignContent=L"stretch";
        float crossOffset=0,dynamicCrossGap=crossGap;
        if(alignContent==L"stretch"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size());
            for(auto& line:lines)line.cross+=extra;crossRemain=0;
        }else if(alignContent==L"center")crossOffset=crossRemain/2;
        else if(alignContent==L"flex-end"||alignContent==L"end")crossOffset=crossRemain;
        else if(alignContent==L"space-between"&&lines.size()>1)
            dynamicCrossGap+=crossRemain/static_cast<float>(lines.size()-1);
        else if(alignContent==L"space-around"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size());
            crossOffset=extra/2;dynamicCrossGap+=extra;
        }else if(alignContent==L"space-evenly"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size()+1);
            crossOffset=extra;dynamicCrossGap+=extra;
        }

        float crossCursor=box.content.y+crossOffset;
        std::vector<size_t> lineOrder;lineOrder.reserve(lines.size());
        if(wrapMode==L"wrap-reverse")
            for(size_t index=lines.size();index>0;--index)lineOrder.push_back(index-1);
        else for(size_t index=0;index<lines.size();++index)lineOrder.push_back(index);
        const auto justify=box.style.Get(L"justify-content");
        for(const auto lineIndex:lineOrder){
            auto& line=lines[lineIndex];
            float occupied=gap*std::max(0,static_cast<int>(line.items.size())-1);
            size_t autoMarginCount=0;
            for(const auto index:line.items){
                occupied+=sizes[index];
                autoMarginCount+=static_cast<size_t>(mainAutoBefore[index])+static_cast<size_t>(mainAutoAfter[index]);
            }
            float remain=std::max(0.0f,mainSize-occupied);
            const float autoMargin=autoMarginCount?remain/static_cast<float>(autoMarginCount):0;
            if(autoMarginCount)remain=0;
            float mainCursor=box.content.x,dynamicGap=gap;
            if(justify==L"center")mainCursor+=remain/2;
            else if(justify==L"flex-end"||justify==L"end")mainCursor+=remain;
            else if(justify==L"space-between"&&line.items.size()>1)
                dynamicGap+=remain/static_cast<float>(line.items.size()-1);
            else if(justify==L"space-around"&&!line.items.empty()){
                const float extra=remain/static_cast<float>(line.items.size());
                mainCursor+=extra/2;dynamicGap+=extra;
            }else if(justify==L"space-evenly"&&!line.items.empty()){
                const float extra=remain/static_cast<float>(line.items.size()+1);
                mainCursor+=extra;dynamicGap+=extra;
            }

            float sharedBaseline=0;
            for(const auto index:line.items)
                if(alignments[index]==L"baseline"&&!crossAutoBefore[index]&&!crossAutoAfter[index])
                    sharedBaseline=std::max(sharedBaseline,FlexItemBaselineOffset(
                        *children[index],sizes[index],crossExtents[index]));
            for(const auto index:line.items){
                auto* child=children[index];const auto& align=alignments[index];
                if(mainAutoBefore[index])mainCursor+=autoMargin;
                const bool autoCross=crossAutoBefore[index]||crossAutoAfter[index];
                const bool stretch=!autoCross&&!crossDefinite[index]&&align==L"stretch";
                const float childCross=stretch?line.cross:std::min(line.cross,crossExtents[index]);
                const float crossFree=std::max(0.0f,line.cross-childCross);
                float childCrossPosition=crossCursor;
                if(crossAutoBefore[index]&&crossAutoAfter[index])childCrossPosition+=crossFree/2;
                else if(crossAutoBefore[index])childCrossPosition+=crossFree;
                else if(!crossAutoAfter[index]&&align==L"center")childCrossPosition+=crossFree/2;
                else if(!crossAutoAfter[index]&&(align==L"flex-end"||align==L"end"))childCrossPosition+=crossFree;
                else if(!crossAutoAfter[index]&&align==L"baseline")
                    childCrossPosition+=sharedBaseline-FlexItemBaselineOffset(
                        *child,sizes[index],childCross);
                LayoutBoxTree(*child,{mainCursor,childCrossPosition,sizes[index],childCross},
                              true,true,stretch||crossDefinite[index]);
                mainCursor+=sizes[index]+(mainAutoAfter[index]?autoMargin:0)+dynamicGap;
            }
            crossCursor+=line.cross+dynamicCrossGap;
        }
        for(auto& child:box.children)if(child->visible&&
            (child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))){
            const LayoutRect area=child->style.Is(L"position",L"fixed")?
                LayoutRect{0,0,viewportWidth_,viewportHeight_}:AbsoluteContainingBlock(box);
            auto positioned=PositionedRect(*child,area,viewportWidth_,viewportHeight_);
            if(child->style.Get(L"left").empty()&&child->style.Get(L"right").empty()&&
               box.style.Is(L"justify-content",L"center"))
                positioned.x=area.x+(area.width-positioned.width)/2;
            if(child->style.Get(L"top").empty()&&child->style.Get(L"bottom").empty()&&
               box.style.Is(L"align-items",L"center"))
                positioned.y=area.y+(area.height-positioned.height)/2;
            LayoutBoxTree(*child,positioned,true);
        }
        return;
    }
    std::vector<float> sizes(children.size()),minimums(children.size()),grows(children.size()),shrinks(children.size()),shrinkWeights(children.size());float fixed=gap*std::max(0,static_cast<int>(children.size())-1),growTotal=0,shrinkTotal=0;
    std::vector<bool> mainAutoBefore(children.size()),mainAutoAfter(children.size()),
        crossAutoBefore(children.size()),crossAutoAfter(children.size());
    size_t mainAutoMarginCount=0;
    for(size_t i=0;i<children.size();++i){
        auto* c=children[i];const auto margin=EdgeValues(c->style,L"margin",column?crossSize:mainSize,viewportWidth_);const float mainMargin=column?margin.top+margin.bottom:margin.left+margin.right;
        const auto isAutoMargin=[&](const wchar_t* property){return ToLower(Trim(c->style.Get(property)))==L"auto";};
        mainAutoBefore[i]=isAutoMargin(column?L"margin-top":L"margin-left");
        mainAutoAfter[i]=isAutoMargin(column?L"margin-bottom":L"margin-right");
        crossAutoBefore[i]=isAutoMargin(column?L"margin-left":L"margin-top");
        crossAutoAfter[i]=isAutoMargin(column?L"margin-right":L"margin-bottom");
        mainAutoMarginCount+=static_cast<size_t>(mainAutoBefore[i])+static_cast<size_t>(mainAutoAfter[i]);
        grows[i]=StyleSheet::Length(c->style.Get(L"flex-grow",L"0"),0,0,0);shrinks[i]=StyleSheet::Length(c->style.Get(L"flex-shrink",L"1"),0,0,1);
        auto raw=c->style.Get(L"flex-basis");if(raw.empty()||raw==L"auto")raw=c->style.Get(column?L"height":L"width");
        bool natural=raw.empty()||raw==L"auto"||(!column&&ToLower(Trim(raw))==L"max-content");const auto minRaw=c->style.Get(column?L"min-height":L"min-width");const auto overflow=c->style.Get(column?L"overflow-y":L"overflow-x",c->style.Get(L"overflow",L"visible"));const bool automaticMinimumIsZero=(!minRaw.empty()&&StyleSheet::Length(minRaw,mainSize,column?viewportHeight_:viewportWidth_,1)==0)||(overflow!=L"visible"&&overflow!=L"clip");float base=natural&&grows[i]>0&&automaticMinimumIsZero?mainMargin:(natural?(column?NaturalHeight(*c,crossSize):NaturalWidth(*c)):StyleSheet::Length(raw,mainSize,column?viewportHeight_:viewportWidth_,0));
        if(!natural){base=column?Constrain(c->style,L"min-height",L"max-height",base,mainSize,viewportHeight_):Constrain(c->style,L"min-width",L"max-width",base,mainSize,viewportWidth_);base+=mainMargin;}
        sizes[i]=base;minimums[i]=column?mainMargin:MinContentWidth(*c);fixed+=base;growTotal+=grows[i];shrinkWeights[i]=shrinks[i]*std::max(0.0f,base-mainMargin);shrinkTotal+=shrinkWeights[i];
    }
    float freeSpace=mainSize-fixed;
    if(freeSpace>0&&growTotal>0){for(size_t index=0;index<children.size();++index)if(grows[index]>0)sizes[index]+=freeSpace*grows[index]/growTotal;}
    else if(freeSpace<0&&shrinkTotal>0){
        float deficit=-freeSpace;std::vector<size_t> active;for(size_t i=0;i<children.size();++i)if(shrinkWeights[i]>0&&sizes[i]>minimums[i]+0.01f)active.push_back(i);
        while(deficit>0.01f&&!active.empty()){
            float weightTotal=0;for(const auto i:active)weightTotal+=shrinkWeights[i];if(weightTotal<=0)break;
            std::vector<size_t> clamped;
            for(const auto i:active){const float reduction=deficit*shrinkWeights[i]/weightTotal;if(sizes[i]-reduction<minimums[i])clamped.push_back(i);}
            if(clamped.empty()){for(const auto i:active)sizes[i]-=deficit*shrinkWeights[i]/weightTotal;deficit=0;break;}
            for(const auto i:clamped){const float reduction=std::max(0.0f,sizes[i]-minimums[i]);sizes[i]=minimums[i];deficit-=reduction;active.erase(std::remove(active.begin(),active.end(),i),active.end());}
        }
    }
    float occupied=gap*std::max(0,static_cast<int>(children.size())-1);for(float size:sizes)occupied+=size;float remain=std::max(0.0f,mainSize-occupied);
    const float autoMainMargin=mainAutoMarginCount&&remain>0?
        remain/static_cast<float>(mainAutoMarginCount):0;
    if(autoMainMargin>0)remain=0;
    float cursor=column?box.content.y:box.content.x;const auto justify=box.style.Get(L"justify-content");float dynamicGap=gap;
    if(justify==L"center")cursor+=remain/2;else if(justify==L"flex-end"||justify==L"end")cursor+=remain;
    else if(justify==L"space-between"&&children.size()>1)dynamicGap+=remain/(children.size()-1);
    else if(justify==L"space-around"&&!children.empty()){const float extra=remain/children.size();cursor+=extra/2;dynamicGap+=extra;}
    else if(justify==L"space-evenly"&&!children.empty()){const float extra=remain/(children.size()+1);cursor+=extra;dynamicGap+=extra;}
    const auto parentAlign=box.style.Get(L"align-items",L"stretch");
    std::vector<std::wstring> alignments(children.size());
    std::vector<float> crossExtents(children.size(),crossSize);
    std::vector<bool> crossDefinite(children.size(),true);
    float sharedBaseline=0;
    for(size_t i=0;i<children.size();++i){
        auto* child=children[i];auto align=child->style.Get(L"align-self",L"auto");
        if(align.empty()||align==L"auto")align=parentAlign;
        alignments[i]=align;
        const auto crossRaw=child->style.Get(column?L"width":L"height");
        if(!crossRaw.empty()&&crossRaw!=L"auto"){
            const auto margin=EdgeValues(child->style,L"margin",crossSize,viewportWidth_);
            crossExtents[i]=StyleSheet::Length(crossRaw,crossSize,
                column?viewportWidth_:viewportHeight_,crossExtents[i])+
                (column?margin.left+margin.right:margin.top+margin.bottom);
        }else if(align!=L"stretch"){
            crossExtents[i]=std::min(crossSize,column?NaturalWidth(*child):
                NaturalHeight(*child,sizes[i]));
            crossDefinite[i]=false;
        }
        if(!column&&align==L"baseline"&&!crossAutoBefore[i]&&!crossAutoAfter[i])
            sharedBaseline=std::max(sharedBaseline,FlexItemBaselineOffset(
                *child,sizes[i],crossExtents[i]));
    }
    for(size_t i=0;i<children.size();++i){
        auto* child=children[i];const auto& align=alignments[i];
        const float cross=crossExtents[i];float crossPos=column?box.content.x:box.content.y;
        const float crossFree=std::max(0.0f,crossSize-cross);
        if(crossAutoBefore[i]&&crossAutoAfter[i])crossPos+=crossFree/2;
        else if(crossAutoBefore[i])crossPos+=crossFree;
        else if(!crossAutoAfter[i]&&align==L"center")crossPos+=crossFree/2;
        else if(!crossAutoAfter[i]&&(align==L"flex-end"||align==L"end"))crossPos+=crossFree;
        else if(!crossAutoAfter[i]&&!column&&align==L"baseline")crossPos+=sharedBaseline-
            FlexItemBaselineOffset(*child,sizes[i],cross);
        if(mainAutoBefore[i])cursor+=autoMainMargin;
        const LayoutRect area=column?LayoutRect{crossPos,cursor,cross,sizes[i]}:
            LayoutRect{cursor,crossPos,sizes[i],cross};
        LayoutBoxTree(*child,area,true,column?crossDefinite[i]:true,
                      column?true:crossDefinite[i]);
        cursor+=sizes[i]+(mainAutoAfter[i]?autoMainMargin:0)+dynamicGap;
    }
    for(auto& c:box.children)if(c->visible&&(c->style.Is(L"position",L"absolute")||c->style.Is(L"position",L"fixed"))){
        const LayoutRect area=c->style.Is(L"position",L"fixed")?LayoutRect{0,0,viewportWidth_,viewportHeight_}:AbsoluteContainingBlock(box);
        auto positioned=PositionedRect(*c,area,viewportWidth_,viewportHeight_);
        if(c->style.Get(L"left").empty()&&c->style.Get(L"right").empty()&&box.style.Is(L"justify-content",L"center"))positioned.x=area.x+(area.width-positioned.width)/2;
        if(c->style.Get(L"top").empty()&&c->style.Get(L"bottom").empty()&&box.style.Is(L"align-items",L"center"))positioned.y=area.y+(area.height-positioned.height)/2;
        LayoutBoxTree(*c,positioned,true);
    }
}

void LayoutEngine::LayoutGrid(LayoutBox& box,bool definiteWidth,bool definiteHeight){
    float gridWidth=box.content.width;
    const auto overflowY=box.style.Get(L"overflow-y",box.style.Get(L"overflow",L"visible"));
    const bool verticalScrollbar=overflowY==L"scroll"||
        (overflowY==L"auto"&&HasStableScrollbarGutter(box.style))||
        (overflowY==L"auto"&&NaturalGridHeight(box,gridWidth)>box.content.height+1);
    if(verticalScrollbar)gridWidth=std::max(0.0f,gridWidth-VerticalScrollbarMetricsFor(box,styleSheet_).width);
    const auto columnDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-columns",L"1fr"),box,gridWidth);
    const auto rowDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-rows"),box,box.content.height);
    size_t areaRows=0,areaColumns=0;const auto areas=ParseGridAreas(box.style.Get(L"grid-template-areas"),areaRows,areaColumns);
    size_t columnCount=std::max<size_t>(1,std::max(columnDefinitions->size(),areaColumns));
    const size_t explicitRows=std::max(areaRows,rowDefinitions->size());
    size_t usedRows=0;const auto items=PlaceGridItems(box,areas,explicitRows,columnCount,usedRows);
    const size_t rowCount=std::max<size_t>(1,std::max(rowDefinitions->size(),usedRows));
    const float columnGap=GapValue(box.style,true,gridWidth,viewportWidth_);
    const float rowGap=GapValue(box.style,false,box.content.height,viewportHeight_);
    const std::vector<float> provisionalRows(rowCount,std::max(1.0f,(box.content.height-rowGap*std::max(0,static_cast<int>(rowCount)-1))/rowCount));
    const auto justifyContent=box.style.Get(L"justify-content",L"normal");
    const auto alignContent=box.style.Get(L"align-content",L"normal");
    const auto columns=ResolveGridTracks(*columnDefinitions,columnCount,gridWidth,columnGap,viewportWidth_,items,true,provisionalRows,definiteWidth,StretchesGridAutoTracks(justifyContent));
    const auto rows=ResolveGridTracks(*rowDefinitions,rowCount,box.content.height,rowGap,viewportHeight_,items,false,columns,definiteHeight,StretchesGridAutoTracks(alignContent));
    const auto horizontalDistribution=DistributeGridContent(justifyContent,gridWidth,columns,columnGap);
    const auto verticalDistribution=DistributeGridContent(alignContent,box.content.height,rows,rowGap);
    const float distributedColumnGap=columnGap+horizontalDistribution.extraGap;
    const float distributedRowGap=rowGap+verticalDistribution.extraGap;
    std::vector<float> x(columns.size()),y(rows.size());float position=box.content.x+horizontalDistribution.offset;
    for(size_t i=0;i<columns.size();++i){x[i]=position;position+=columns[i]+distributedColumnGap;}
    position=box.content.y+verticalDistribution.offset;for(size_t i=0;i<rows.size();++i){y[i]=position;position+=rows[i]+distributedRowGap;}
    const auto parentAlign=box.style.Get(L"align-items",L"stretch"),parentJustify=box.style.Get(L"justify-items",L"stretch");
    for(const auto& item:items){
        if(item.row>=rows.size()||item.column>=columns.size())continue;
        float cellWidth=distributedColumnGap*std::max(0,static_cast<int>(item.columnSpan)-1),cellHeight=distributedRowGap*std::max(0,static_cast<int>(item.rowSpan)-1);
        for(size_t index=0;index<item.columnSpan&&item.column+index<columns.size();++index)cellWidth+=columns[item.column+index];
        for(size_t index=0;index<item.rowSpan&&item.row+index<rows.size();++index)cellHeight+=rows[item.row+index];
        auto align=item.box->style.Get(L"align-self",L"auto");if(align.empty()||align==L"auto")align=parentAlign;
        auto justify=item.box->style.Get(L"justify-self",L"auto");if(justify.empty()||justify==L"auto")justify=parentJustify;
        const auto cssHeight=item.box->style.Get(L"height"),cssWidth=item.box->style.Get(L"width");
        float width=cellWidth,height=cellHeight,left=x[item.column],top=y[item.row];
        if(!cssWidth.empty()&&cssWidth!=L"auto")
            width=std::min(cellWidth,BlockOuterWidth(*item.box,cellWidth,viewportWidth_));
        else if(justify!=L"stretch")width=std::min(cellWidth,NaturalWidth(*item.box));
        if(!cssHeight.empty()&&cssHeight!=L"auto")
            height=std::min(cellHeight,BlockOuterHeight(*item.box,cellHeight,cellWidth,viewportHeight_,viewportWidth_));
        else if(align!=L"stretch")height=std::min(cellHeight,NaturalHeight(*item.box,width));
        const bool autoLeft=ToLower(Trim(item.box->style.Get(L"margin-left")))==L"auto";
        const bool autoRight=ToLower(Trim(item.box->style.Get(L"margin-right")))==L"auto";
        const bool autoTop=ToLower(Trim(item.box->style.Get(L"margin-top")))==L"auto";
        const bool autoBottom=ToLower(Trim(item.box->style.Get(L"margin-bottom")))==L"auto";
        if(autoLeft&&autoRight)left+=(cellWidth-width)/2;
        else if(autoLeft)left+=cellWidth-width;
        else if(justify==L"center")left+=(cellWidth-width)/2;
        else if(justify==L"end"||justify==L"flex-end")left+=cellWidth-width;
        if(autoTop&&autoBottom)top+=(cellHeight-height)/2;
        else if(autoTop)top+=cellHeight-height;
        else if(align==L"center")top+=(cellHeight-height)/2;
        else if(align==L"end"||align==L"flex-end")top+=cellHeight-height;
        LayoutBoxTree(*item.box,{left,top,width,height},true);
    }
    for(auto& child:box.children)if(child->visible&&(child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))){
        auto area=child->style.Is(L"position",L"fixed")?LayoutRect{0,0,viewportWidth_,viewportHeight_}:AbsoluteContainingBlock(box);
        if(!child->style.Is(L"position",L"fixed"))
            area.width=std::max(0.0f,area.width-(box.content.width-gridWidth));
        LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);
    }
}

void LayoutEngine::LayoutTable(LayoutBox& box){
    const auto model=BuildTableGrid(box);
    if(!model.columnCount||model.rows.empty()){LayoutBlock(box);return;}
    const auto columns=ResolveTableColumns(box,model,box.content.width,viewportWidth_);
    auto rows=ResolveTableRows(model,columns);
    float rowsHeight=0;for(const auto height:rows)rowsHeight+=height;
    if(rowsHeight>0&&box.content.height>rowsHeight+0.01f){
        const float share=(box.content.height-rowsHeight)/static_cast<float>(rows.size());
        for(auto& height:rows)height+=share;
    }

    std::vector<float> x(columns.size()),y(rows.size());
    float position=box.content.x;
    for(size_t column=0;column<columns.size();++column){
        x[column]=position;position+=columns[column];
    }
    position=box.content.y;
    for(size_t row=0;row<rows.size();++row){
        y[row]=position;
        auto* rowBox=model.rows[row].box;
        rowBox->rect={box.content.x,position,box.content.width,rows[row]};
        rowBox->content=rowBox->rect;
        position+=rows[row];
    }
    for(const auto& cell:model.cells){
        if(cell.row>=rows.size()||cell.column>=columns.size())continue;
        float width=0,height=0;
        for(size_t column=cell.column;
            column<std::min(columns.size(),cell.column+cell.columnSpan);++column)
            width+=columns[column];
        for(size_t row=cell.row;row<std::min(rows.size(),cell.row+cell.rowSpan);++row)
            height+=rows[row];
        LayoutBoxTree(*cell.box,{x[cell.column],y[cell.row],width,height},true);
    }
    std::function<bool(LayoutBox&,LayoutRect&)> fitGroups=
        [&](LayoutBox& current,LayoutRect& bounds){
            if(&current!=&box&&HasTableDisplay(current,L"table"))return false;
            if(HasTableDisplay(current,L"table-row")){bounds=current.rect;return true;}
            bool found=false;float left=0,top=0,right=0,bottom=0;
            for(auto& child:current.children){
                LayoutRect childBounds;if(!fitGroups(*child,childBounds))continue;
                if(!found){left=childBounds.x;top=childBounds.y;
                    right=childBounds.x+childBounds.width;
                    bottom=childBounds.y+childBounds.height;found=true;}
                else{left=std::min(left,childBounds.x);top=std::min(top,childBounds.y);
                    right=std::max(right,childBounds.x+childBounds.width);
                    bottom=std::max(bottom,childBounds.y+childBounds.height);}
            }
            if(found&&IsTableRowGroup(current)){
                current.rect={left,top,right-left,bottom-top};current.content=current.rect;
            }
            if(found)bounds={left,top,right-left,bottom-top};
            return found;
        };
    LayoutRect ignored;for(auto& child:box.children)fitGroups(*child,ignored);
}

void LayoutEngine::Paint(ID2D1RenderTarget* target,IDWriteFactory* factory){if(!root_||!target||!factory)return;PaintStackingContext(target,factory,*root_,{0,0,viewportWidth_,viewportHeight_});}

void LayoutEngine::PaintStackingContext(ID2D1RenderTarget* target,IDWriteFactory* factory,
                                        LayoutBox& box,const LayoutRect& clipBounds){
    target->PushAxisAlignedClip(PixelAlignedRect(clipBounds),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    PaintBox(target,factory,box,clipBounds,&box.nonNegativeStackingContexts);
    for(auto* context:box.nonNegativeStackingContexts)
        PaintStackingContext(target,factory,*context,StackingContextClip(*context,box,clipBounds));
    target->PopAxisAlignedClip();
}

void LayoutEngine::PaintBox(ID2D1RenderTarget* target,IDWriteFactory* factory,LayoutBox& box,
                            const LayoutRect& clipBounds,
                            const std::vector<LayoutBox*>* deferredContexts){
    if(IsDeferredContext(box,deferredContexts))return;
    const bool intersects=box.rect.x<clipBounds.x+clipBounds.width&&box.rect.x+box.rect.width>clipBounds.x&&box.rect.y<clipBounds.y+clipBounds.height&&box.rect.y+box.rect.height>clipBounds.y;
    if(!box.visible||box.rect.width<=0||box.rect.height<=0||!intersects)return;
    float opacity=1.0f;try{opacity=std::stof(box.style.Get(L"opacity",L"1"));}catch(...){}
    opacity=std::max(0.0f,std::min(1.0f,opacity));
    if(box.style.Is(L"visibility",L"hidden")||opacity<=0.001f)return;D2D1_MATRIX_3X2_F previousTransform{};const bool transformed=ApplyPaintTransform(target,box,previousTransform);Microsoft::WRL::ComPtr<ID2D1Layer> opacityLayer;if(opacity<0.999f&&SUCCEEDED(target->CreateLayer(nullptr,&opacityLayer)))target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),opacity),opacityLayer.Get());const auto background=BackgroundColor(box.style);Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    const auto radius=UniformCornerRadii(box.style,box.rect.width,box.rect.height,viewportWidth_);
    PaintOuterBoxShadows(target,box.style,box.rect,radius,viewportWidth_);
    if((background>>24)!=0){target->CreateSolidColorBrush(D2DColor(background),&brush);auto rect=PixelAlignedRect(box.rect);if(radius.x>0&&radius.y>0)target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius.x,radius.y),brush.Get());else target->FillRectangle(rect,brush.Get());}
    PaintGradientBackgrounds(target,box.style,box.rect,radius,viewportWidth_);
    PaintInsetBoxShadows(target,box.style,box.rect,viewportWidth_);
    const auto borders=BorderValues(box.style);const bool uniform=borders.top==borders.right&&borders.top==borders.bottom&&borders.top==borders.left;
    bool collapsedTableCell=false;
    if(HasTableDisplay(box,L"table-cell"))for(auto* ancestor=box.parent;ancestor;ancestor=ancestor->parent)if(HasTableDisplay(*ancestor,L"table")){collapsedTableCell=ancestor->style.Is(L"border-collapse",L"collapse");break;}
    if(uniform&&borders.top>0){auto color=BorderColor(box.style,L"top");target->CreateSolidColorBrush(D2DColor(color),&brush);const float inset=borders.top/2;auto rect=D2D1::RectF(std::round(box.rect.x)+inset,std::round(box.rect.y)+inset,std::round(box.rect.x+box.rect.width)-inset,std::round(box.rect.y+box.rect.height)-inset);if(radius.x>0&&radius.y>0){const float strokeRadiusX=std::max(0.0f,radius.x-inset),strokeRadiusY=std::max(0.0f,radius.y-inset);target->DrawRoundedRectangle(D2D1::RoundedRect(rect,strokeRadiusX,strokeRadiusY),brush.Get(),borders.top);}else target->DrawRectangle(rect,brush.Get(),borders.top);}
    else{
        FLOAT dpiX=USER_DEFAULT_SCREEN_DPI,dpiY=USER_DEFAULT_SCREEN_DPI;target->GetDpi(&dpiX,&dpiY);
        const auto pixelCenter=[](float value,float dpi){const float scale=dpi/USER_DEFAULT_SCREEN_DPI;return scale>0?(std::round(value*scale-0.5f)+0.5f)/scale:value;};
        if(borders.top>0){target->CreateSolidColorBrush(D2DColor(BorderColor(box.style,L"top")),&brush);const float y=pixelCenter(std::round(box.rect.y)+borders.top/2,dpiY);target->DrawLine(D2D1::Point2F(std::round(box.rect.x),y),D2D1::Point2F(std::round(box.rect.x+box.rect.width),y),brush.Get(),borders.top);}
        if(borders.right>0){target->CreateSolidColorBrush(D2DColor(BorderColor(box.style,L"right")),&brush);const float x=pixelCenter(std::round(box.rect.x+box.rect.width)-borders.right/2,dpiX);target->DrawLine(D2D1::Point2F(x,std::round(box.rect.y)),D2D1::Point2F(x,std::round(box.rect.y+box.rect.height)),brush.Get(),borders.right);}
        if(borders.bottom>0){target->CreateSolidColorBrush(D2DColor(BorderColor(box.style,L"bottom")),&brush);const float y=pixelCenter(std::round(box.rect.y+box.rect.height)+(collapsedTableCell?borders.bottom/2:-borders.bottom/2),dpiY);target->DrawLine(D2D1::Point2F(std::round(box.rect.x),y),D2D1::Point2F(std::round(box.rect.x+box.rect.width),y),brush.Get(),borders.bottom);}
        if(borders.left>0){target->CreateSolidColorBrush(D2DColor(BorderColor(box.style,L"left")),&brush);const float x=pixelCenter(std::round(box.rect.x)+borders.left/2,dpiX);target->DrawLine(D2D1::Point2F(x,std::round(box.rect.y)),D2D1::Point2F(x,std::round(box.rect.y+box.rect.height)),brush.Get(),borders.left);}
    }
    if(box.node->tag==L"input"&&(box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio")){
        const auto type=box.node->Attribute(L"type");
        const float size=std::max(1.0f,std::min(box.rect.width,box.rect.height));
        const float left=std::round(box.rect.x),top=std::round(box.rect.y+(box.rect.height-size)/2);
        const auto r=D2D1::RectF(left,top,left+size,top+size);
        const auto accent=box.node->disabled?0xff9ca3af:StyleSheet::Color(box.style.Get(L"accent-color",L"#0d73d8"),0xff0d73d8);
        if(type==L"radio"){
            target->CreateSolidColorBrush(D2DColor(0xff6b7280),&brush);
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2),size/2-1,size/2-1),brush.Get(),1);
            if(box.node->checked){target->CreateSolidColorBrush(D2DColor(accent),&brush);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2),4,4),brush.Get());}
        }else{
            const float controlRadius=std::max(1.5f,size*0.15f);
            if(box.node->checked){
                target->CreateSolidColorBrush(D2DColor(accent),&brush);
                target->FillRoundedRectangle(D2D1::RoundedRect(r,controlRadius,controlRadius),brush.Get());
                target->CreateSolidColorBrush(D2DColor(0xffffffff),&brush);
                const float scale=size/13.0f,stroke=std::max(1.5f,size*0.14f);
                const auto middle=D2D1::Point2F(r.left+5.25f*scale,r.top+9.25f*scale);
                target->DrawLine(D2D1::Point2F(r.left+2.5f*scale,r.top+6.5f*scale),middle,brush.Get(),stroke);
                target->DrawLine(middle,D2D1::Point2F(r.left+10.5f*scale,r.top+3.5f*scale),brush.Get(),stroke);
            }else{
                target->CreateSolidColorBrush(D2DColor(box.node->disabled?0xffb8bec6:0xff6b7280),&brush);
                target->FillRoundedRectangle(D2D1::RoundedRect(r,controlRadius,controlRadius),brush.Get());
                const auto inner=D2D1::RectF(r.left+1,r.top+1,r.right-1,r.bottom-1);
                target->CreateSolidColorBrush(D2DColor(box.node->disabled?0xfff3f4f6:0xffffffff),&brush);
                target->FillRoundedRectangle(D2D1::RoundedRect(inner,std::max(0.5f,controlRadius-1),std::max(0.5f,controlRadius-1)),brush.Get());
            }
        }
    }
    if(box.node->tag==L"svg")PaintSvg(target,box,styleSheet_);
    bool placeholderText=false;std::wstring text=BoxText(box,&placeholderText);
    if(box.node->type==NodeType::Text&&!text.empty()){
        const auto* owner=box.parent;
        if(owner&&owner->style.Is(L"text-overflow",L"ellipsis")&&
           owner->style.Is(L"white-space",L"nowrap")&&
           (owner->style.Is(L"overflow",L"hidden")||owner->style.Is(L"overflow-x",L"hidden"))){
            const float available=std::max(0.0f,clipBounds.x+clipBounds.width-box.content.x);
            if(TextWidth(text,box.style)>available+0.5f){
                const std::wstring marker=L"\u2026";
                if(TextWidth(marker,box.style)>available)text.clear();
                else{
                    size_t low=0,high=text.size();
                    while(low<high){
                        const size_t middle=(low+high+1)/2;
                        if(TextWidth(text.substr(0,middle)+marker,box.style)<=available)low=middle;
                        else high=middle-1;
                    }
                    if(low>0&&low<text.size()&&text[low-1]>=0xd800&&text[low-1]<=0xdbff)--low;
                    text=text.substr(0,low)+marker;
                }
            }
        }
    }
    if(!text.empty()){
        const bool formControl=IsFormControlText(box);
        EnsureTextLayout(box,factory,text);
        if(box.textLayout){
            auto textColor=StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000);
            float textOpacity=1.0f;
            if(placeholderText){
                const auto placeholderStyle=styleSheet_.Compute(box.node,&box.style,L"placeholder");
                textColor=StyleSheet::Color(placeholderStyle.Get(L"color"),0xff757575);
                try{textOpacity=std::stof(placeholderStyle.Get(L"opacity",L"1"));}catch(...){ }
                textOpacity=std::max(0.0f,std::min(1.0f,textOpacity));
            }
            auto resolvedTextColor=D2DColor(textColor);resolvedTextColor.a*=textOpacity;
            target->CreateSolidColorBrush(resolvedTextColor,&brush);
            const auto rect=D2D1::RectF(box.content.x,box.content.y,box.content.x+box.content.width,box.content.y+box.content.height);
            // A glyph's ink may extend beyond its advance, especially with
            // negative character spacing. Only CSS overflow clips text ink.
            const auto textClip=box.node->type==NodeType::Text?
                D2D1::RectF(clipBounds.x,clipBounds.y,
                            clipBounds.x+clipBounds.width,clipBounds.y+clipBounds.height):rect;
            target->PushAxisAlignedClip(textClip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const auto origin=TextOrigin(box);
            float textLeft=origin.x;
            float textTop=origin.y;
            if(formControl){
                FLOAT dpiX=USER_DEFAULT_SCREEN_DPI,dpiY=USER_DEFAULT_SCREEN_DPI;target->GetDpi(&dpiX,&dpiY);
                // Center decorative glyphs on a physical half-pixel so their
                // symmetric strokes rasterize like browser toolbar icons at
                // every display scale without a CSS-pixel offset.
                if(dpiX>0&&IsDecorativeControlText(box.node))
                    textLeft+=0.5f*USER_DEFAULT_SCREEN_DPI/dpiX;
                if(dpiY>0){
                    float pixelTop=std::round(textTop*dpiY/USER_DEFAULT_SCREEN_DPI);
                    // DirectWrite's GDI-compatible layout uses a one-device-pixel
                    // higher ink origin than the Windows browser button baseline.
                    // Apply the baseline correction in physical pixels so it is
                    // stable at every display scale instead of becoming a CSS-px
                    // adjustment tied to one viewport or control.
                    // Decorative symbols keep the font's centered glyph origin;
                    // the one-pixel browser baseline correction belongs to labels.
                    if(IsButtonControlText(box.node)&&!IsDecorativeControlText(box.node))pixelTop+=1.0f;
                    textTop=pixelTop*USER_DEFAULT_SCREEN_DPI/dpiY;
                }
            }
            target->DrawTextLayout(D2D1::Point2F(textLeft,textTop),box.textLayout.Get(),brush.Get());
            target->PopAxisAlignedClip();
        }
    }
    if(box.node->tag==L"select"){
        const float centerX=box.rect.x+box.rect.width-9.5f,centerY=box.rect.y+box.rect.height/2.0f;
        target->CreateSolidColorBrush(D2DColor(StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000)),&brush);
        Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;target->GetFactory(&d2dFactory);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arrow;
        if(d2dFactory&&SUCCEEDED(d2dFactory->CreatePathGeometry(&arrow))){
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(arrow->Open(&sink))){
                sink->BeginFigure(D2D1::Point2F(centerX-4,centerY-2),D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(centerX+5,centerY-2));
                sink->AddLine(D2D1::Point2F(centerX,centerY+3));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);sink->Close();target->FillGeometry(arrow.Get(),brush.Get());
            }
        }
    }
    const auto overflow=box.style.Get(L"overflow",L"visible"),overflowX=box.style.Get(L"overflow-x",L"visible"),overflowY=box.style.Get(L"overflow-y",L"visible");const bool clip=overflow==L"hidden"||overflow==L"clip"||overflow==L"auto"||overflow==L"scroll"||overflowX==L"hidden"||overflowX==L"clip"||overflowX==L"auto"||overflowX==L"scroll"||overflowY==L"hidden"||overflowY==L"clip"||overflowY==L"auto"||overflowY==L"scroll";LayoutRect childClip=clipBounds;if(clip){const float left=std::max(clipBounds.x,box.content.x),top=std::max(clipBounds.y,box.content.y),right=std::min(clipBounds.x+clipBounds.width,box.content.x+box.content.width),bottom=std::min(clipBounds.y+clipBounds.height,box.content.y+box.content.height);childClip={left,top,std::max(0.0f,right-left),std::max(0.0f,bottom-top)};target->PushAxisAlignedClip(PixelAlignedRect(box.content),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);}
    const auto paintChild=[&](LayoutBox& child){
        if(IsDeferredContext(child,deferredContexts))return;
        if(IsStackingContext(child))PaintStackingContext(target,factory,child,childClip);
        else PaintBox(target,factory,child,childClip,deferredContexts);
    };
    if(!box.verticallyOrderedChildren.empty()){
        const float top=childClip.y,bottom=childClip.y+childClip.height;
        auto first=std::lower_bound(box.verticallyOrderedChildren.begin(),box.verticallyOrderedChildren.end(),top,
            [](const LayoutBox* child,float value){return child->rect.y+child->rect.height<=value;});
        for(auto it=first;it!=box.verticallyOrderedChildren.end()&&(*it)->rect.y<bottom;++it)paintChild(**it);
        for(auto* child:box.overlayChildren)paintChild(*child);
    }else{
        for(auto* child:box.paintChildren)paintChild(*child);
    }
    if(clip)target->PopAxisAlignedClip();
    VerticalScrollbarGeometry scrollbar;if(VerticalScrollbarFor(box,styleSheet_,scrollbar)){
        const auto colorScheme=ToLower(Trim(box.style.Get(L"color-scheme",L"light")));
        const bool darkScheme=!colorScheme.empty()&&Words(colorScheme).front()==L"dark";
        unsigned int thumbColor=darkScheme?0xff9f9f9f:0xff8b8b8b;
        unsigned int trackColor=darkScheme?0xff2c2c2c:0xfffcfcfc;
        const auto colors=Words(box.style.Get(L"scrollbar-color"));
        if(colors.size()>=2){thumbColor=StyleSheet::Color(colors[0],thumbColor);trackColor=StyleSheet::Color(colors[1],trackColor);}
        const auto trackStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-track")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-track"):ComputedStyle{};
        const auto trackBackground=trackStyle.Get(L"background-color",trackStyle.Get(L"background"));
        if(!scrollbar.standardStyling&&!trackBackground.empty())trackColor=StyleSheet::Color(trackBackground,trackColor);
        const auto thumbStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-thumb")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
        const auto thumbBackground=thumbStyle.Get(L"background-color",thumbStyle.Get(L"background"));
        if(!scrollbar.standardStyling&&!thumbBackground.empty())thumbColor=StyleSheet::Color(thumbBackground,thumbColor);
        const float trackBottom=scrollbar.track.y+scrollbar.track.height;
        if((trackColor>>24)!=0){target->CreateSolidColorBrush(D2DColor(trackColor),&brush);target->FillRectangle(D2D1::RectF(scrollbar.track.x,scrollbar.track.y,scrollbar.track.x+scrollbar.track.width,trackBottom),brush.Get());}
        target->CreateSolidColorBrush(D2DColor(thumbColor),&brush);const auto thumbRadii=UniformCornerRadii(thumbStyle,scrollbar.thumb.width,scrollbar.thumb.height,viewportWidth_);const float thumbRadius=scrollbar.standardStyling?std::min(scrollbar.thumb.width,scrollbar.thumb.height)/2.0f:(thumbRadii.x>0?thumbRadii.x:std::min(scrollbar.thumb.width,scrollbar.thumb.height)/2.0f);target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(scrollbar.thumb.x,scrollbar.thumb.y,scrollbar.thumb.x+scrollbar.thumb.width,scrollbar.thumb.y+scrollbar.thumb.height),thumbRadius,thumbRadius),brush.Get());
        if(scrollbar.arrowHeight>0){
            const float center=scrollbar.track.x+scrollbar.track.width/2;
            {
                Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;target->GetFactory(&d2dFactory);
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> arrows;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if(d2dFactory&&SUCCEEDED(d2dFactory->CreatePathGeometry(&arrows))&&SUCCEEDED(arrows->Open(&sink))){
                    const float halfWidth=scrollbar.thumb.width/2.0f;
                    const float arrowFigureHeight=scrollbar.arrowHeight/3.0f;
                    const float arrowPadding=(scrollbar.arrowHeight-arrowFigureHeight)/2.0f;
                    const float topApex=scrollbar.track.y+arrowPadding;
                    const float topBase=topApex+arrowFigureHeight;
                    const float bottomBase=trackBottom-arrowPadding-arrowFigureHeight;
                    const float bottomApex=trackBottom-arrowPadding;
                    sink->BeginFigure(D2D1::Point2F(center,topApex),D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(center-halfWidth,topBase));sink->AddLine(D2D1::Point2F(center+halfWidth,topBase));sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->BeginFigure(D2D1::Point2F(center-halfWidth,bottomBase),D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(center+halfWidth,bottomBase));sink->AddLine(D2D1::Point2F(center,bottomApex));sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();target->FillGeometry(arrows.Get(),brush.Get());
                }
            }
        }
    }
    if(opacityLayer)target->PopLayer();
    if(transformed)target->SetTransform(previousTransform);
}

std::shared_ptr<Node> LayoutEngine::HitTest(float x,float y)const{return root_?HitTestStackingContext(*root_,x,y):nullptr;}

bool LayoutEngine::HitTestText(const std::shared_ptr<Node>& scope,float x,float y,
                               std::shared_ptr<Node>& textNode,size_t& textOffset){
    textNode.reset();textOffset=0;if(!scope||!root_)return false;
    const auto scopeEntry=boxIndex_.find(scope.get());if(scopeEntry==boxIndex_.end())return false;
    LayoutBox* best=nullptr;std::shared_ptr<Node> bestSource;
    float bestDistance=std::numeric_limits<float>::max();
    const LayoutRect viewport{0,0,viewportWidth_,viewportHeight_};
    std::function<void(LayoutBox&,LayoutRect,bool)> visit=
        [&](LayoutBox& box,LayoutRect clip,bool generated){
            if(!box.visible)return;
            generated=generated||!box.pseudo.empty();
            if(ClipsOverflow(box))clip=IntersectRects(clip,box.content);
            const bool textRun=box.node->type==NodeType::Text;
            const bool textControl=box.node==scope&&(box.node->tag==L"input"||box.node->tag==L"textarea");
            if(!generated&&(textRun||textControl)){
                auto source=textRun&&box.generatedFrom&&box.generatedFrom->type==NodeType::Text?
                    box.generatedFrom:box.node;
                const auto visible=IntersectRects(box.rect,clip);
                if(source&&visible.width>0&&visible.height>0){
                    const float dx=x<visible.x?visible.x-x:(x>visible.x+visible.width?x-visible.x-visible.width:0);
                    const float dy=y<visible.y?visible.y-y:(y>visible.y+visible.height?y-visible.y-visible.height:0);
                    // A line under the pointer wins over horizontally closer
                    // text on another line, matching browser caret placement.
                    const float distance=dy*dy*16.0f+dx*dx;
                    if(distance<bestDistance){bestDistance=distance;best=&box;bestSource=std::move(source);}
                }
            }
            if(generated)return;
            for(auto& child:box.children)visit(*child,clip,generated);
        };
    visit(*scopeEntry->second,viewport,false);
    if(!best||!bestSource)return false;
    const auto text=BoxText(*best);
    EnsureTextLayout(*best,SharedWriteFactory(),text);
    size_t localOffset=text.size();
    if(best->textLayout){
        const auto origin=TextOrigin(*best);BOOL trailing=FALSE,inside=FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        if(SUCCEEDED(best->textLayout->HitTestPoint(x-origin.x,y-origin.y,
            &trailing,&inside,&metrics)))
            localOffset=std::min(text.size(),static_cast<size_t>(metrics.textPosition)+
                (trailing?static_cast<size_t>(metrics.length):0));
    }
    const size_t sourceLength=bestSource->type==NodeType::Text?bestSource->text.size():
        bestSource->Attribute(L"value").size();
    const size_t sourceOffset=bestSource->type==NodeType::Text?best->textSourceOffset:0;
    textNode=bestSource;textOffset=std::min(sourceLength,sourceOffset+localOffset);return true;
}

bool LayoutEngine::TextCaretRect(const std::shared_ptr<Node>& textNode,size_t textOffset,
                                 LayoutRect& caretRect){
    caretRect={};if(!textNode||!root_)return false;
    LayoutBox* matched=nullptr;std::wstring matchedText;
    std::function<void(LayoutBox&,bool)> find=[&](LayoutBox& box,bool generated){
        if(matched||!box.visible)return;
        generated=generated||!box.pseudo.empty();
        if(!generated){
            auto source=box.node->type==NodeType::Text&&box.generatedFrom&&
                box.generatedFrom->type==NodeType::Text?box.generatedFrom:box.node;
            if(source==textNode){
                const auto text=BoxText(box);
                const size_t start=textNode->type==NodeType::Text?box.textSourceOffset:0;
                if(textOffset>=start&&textOffset<=start+text.size()){
                    matched=&box;matchedText=text;return;
                }
            }
        }
        if(generated)return;
        for(auto& child:box.children)find(*child,generated);
    };
    find(*root_,false);
    if(matched){
        // An empty single-line input still owns a centered text line. Measure
        // that line with a zero-width probe so its initial caret uses exactly
        // the same font metrics and paragraph alignment as entered text.
        const auto caretText=matchedText.empty()&&matched->node->tag==L"input"?
            std::wstring(1,L'\x200b'):matchedText;
        EnsureTextLayout(*matched,SharedWriteFactory(),caretText);
        if(matched->textLayout){
            const size_t start=textNode->type==NodeType::Text?matched->textSourceOffset:0;
            const UINT32 local=matchedText.empty()?0:static_cast<UINT32>(
                std::min(matchedText.size(),textOffset-start));
            FLOAT hitX=0,hitY=0;DWRITE_HIT_TEST_METRICS metrics{};
            if(SUCCEEDED(matched->textLayout->HitTestTextPosition(local,FALSE,&hitX,&hitY,&metrics))){
                const auto origin=TextOrigin(*matched);
                caretRect={origin.x+hitX,origin.y+hitY,1.0f/std::max(0.01f,deviceScale_),
                    std::max(1.0f,metrics.height)};
                return true;
            }
        }
    }
    // Empty and wholly-collapsed text nodes intentionally have no CSS text
    // box. Their insertion point is the start of the nearest rendered
    // ancestor's content box, with that ancestor's computed line height.
    for(auto current=textNode;current;current=current->parent.lock()){
        const auto found=boxIndex_.find(current.get());if(found==boxIndex_.end())continue;
        const auto* box=found->second;if(!box||!box->visible)continue;
        const float lineHeight=std::max(1.0f,LineHeight(box->style));
        float top=box->content.y-(box->node->tag==L"textarea"?box->node->scrollTop:0.0f);
        // A single-line control centers its line box even before it contains
        // text. Keep the empty-control fallback on the same vertical track as
        // DirectWrite's centered paragraph used after the first character.
        if(box->node->tag==L"input")
            top+=std::max(0.0f,(box->content.height-lineHeight)/2.0f);
        caretRect={box->content.x,top,1.0f/std::max(0.01f,deviceScale_),lineHeight};
        return true;
    }
    return false;
}

std::shared_ptr<Node> LayoutEngine::HitTestStackingContext(const LayoutBox& box,float x,float y)const{
    for(auto it=box.nonNegativeStackingContexts.rbegin();it!=box.nonNegativeStackingContexts.rend();++it){
        const auto* context=*it;
        if(StackingContextAllowsPoint(*context,box,x,y))
            if(auto node=HitTestStackingContext(*context,x,y))return node;
    }
    return HitTestBox(box,x,y);
}
std::shared_ptr<Node> LayoutEngine::HitTestBox(const LayoutBox& box,float x,float y)const{
    if(!box.visible)return {};
    const bool inside=box.rect.Contains(x,y);
    const auto overflow=box.style.Get(L"overflow",L"visible");
    const auto overflowX=box.style.Get(L"overflow-x",overflow),overflowY=box.style.Get(L"overflow-y",overflow);
    const bool clips=overflow==L"hidden"||overflow==L"clip"||overflow==L"auto"||overflow==L"scroll"||
        overflowX==L"hidden"||overflowX==L"clip"||overflowX==L"auto"||overflowX==L"scroll"||
        overflowY==L"hidden"||overflowY==L"clip"||overflowY==L"auto"||overflowY==L"scroll";
    if(!inside&&clips)return {};
    if(!box.verticallyOrderedChildren.empty()){
        for(auto it=box.overlayChildren.rbegin();it!=box.overlayChildren.rend();++it)
            if(auto node=HitTestBox(**it,x,y))return node;
        auto first=std::lower_bound(box.verticallyOrderedChildren.begin(),box.verticallyOrderedChildren.end(),y,
            [](const LayoutBox* child,float value){return child->rect.y+child->rect.height<=value;});
        auto last=first;
        while(last!=box.verticallyOrderedChildren.end()&&(*last)->rect.y<=y)++last;
        while(last!=first){--last;if(auto node=HitTestBox(**last,x,y))return node;}
    }else{
        for(auto it=box.paintChildren.rbegin();it!=box.paintChildren.rend();++it)
            if(auto node=HitTestBox(**it,x,y))return node;
    }
    if(!inside||box.style.Is(L"pointer-events",L"none")||box.style.Is(L"visibility",L"hidden"))return {};
    if(box.generatedFrom)return box.generatedFrom;
    return box.node&&box.node->type==NodeType::Element?box.node:box.node->parent.lock();
}
const LayoutBox* LayoutEngine::BoxFor(const std::shared_ptr<Node>& node)const{if(!root_||!node)return nullptr;const auto found=boxIndex_.find(node.get());return found==boxIndex_.end()?nullptr:found->second;}
bool LayoutEngine::Restyle(const std::shared_ptr<Node>& node){if(!root_||!node)return false;const auto found=boxIndex_.find(node.get());if(found==boxIndex_.end())return true;auto* box=found->second;const ComputedStyle* parentStyle=nullptr;if(auto parent=node->parent.lock()){const auto parentBox=boxIndex_.find(parent.get());if(parentBox!=boxIndex_.end())parentStyle=&parentBox->second->style;}return RestyleBox(*box,parentStyle);}
bool LayoutEngine::RestyleBox(LayoutBox& box,const ComputedStyle* parentStyle){auto updated=box.generatedFrom?styleSheet_.Compute(box.generatedFrom,parentStyle,box.pseudo):styleSheet_.Compute(box.node,parentStyle);updated.deviceScale=deviceScale_;bool layoutChanged=HasLayoutStyleChange(box.style,updated);box.style=updated;for(auto& child:box.children)layoutChanged=RestyleBox(*child,&box.style)||layoutChanged;return layoutChanged;}
bool LayoutEngine::ScrollAt(float x,float y,float wheelDelta,std::shared_ptr<Node>* scrolledNode){
    if(scrolledNode)scrolledNode->reset();
    if(!root_)return false;
    const auto target=HitTest(x,y);
    if(!target)return false;
    const auto found=boxIndex_.find(target.get());
    if(found==boxIndex_.end())return false;
    // Wheel scrolling follows the painted hit target's ancestor chain.  Searching
    // every box under the coordinates lets an exhausted popup fall through to a
    // covered sibling (for example, the page behind a menu), which browsers do
    // not include in the scroll chain.
    for(auto* box=found->second;box;box=box->parent)
        if(ScrollBox(*box,x,y,wheelDelta,scrolledNode))return true;
    return false;
}
bool LayoutEngine::ScrollBox(LayoutBox& box,float x,float y,float wheelDelta,std::shared_ptr<Node>* scrolledNode){if(!box.visible)return false;const auto overflowY=box.style.Get(L"overflow-y",L"visible");if((overflowY==L"auto"||overflowY==L"scroll")&&box.content.Contains(x,y)&&box.scrollHeight>box.content.height+1){const float maximum=std::max(0.0f,box.scrollHeight-box.content.height),old=box.node->scrollTop;box.node->scrollTop=std::max(0.0f,std::min(maximum,old-wheelDelta/120.0f*90.0f));ApplyScrollOffset(box,old,viewportHeight_);if(box.node->scrollTop!=old){if(scrolledNode)*scrolledNode=box.node;return true;}}return false;}
bool LayoutEngine::BeginScrollbarInteraction(float x,float y,std::shared_ptr<Node>& dragNode,float& dragOffset){
    dragNode.reset();dragOffset=0;if(!root_)return false;
    for(auto it=root_->nonNegativeStackingContexts.rbegin();it!=root_->nonNegativeStackingContexts.rend();++it)
        if(StackingContextAllowsPoint(**it,*root_,x,y)&&BeginScrollbarBox(**it,x,y,dragNode,dragOffset))return true;
    return BeginScrollbarBox(*root_,x,y,dragNode,dragOffset);
}
bool LayoutEngine::BeginScrollbarBox(LayoutBox& box,float x,float y,std::shared_ptr<Node>& dragNode,float& dragOffset){
    if(!box.visible||!box.rect.Contains(x,y))return false;VerticalScrollbarGeometry geometry;
    if(VerticalScrollbarFor(box,styleSheet_,geometry)&&geometry.track.Contains(x,y)){
        if(y>=geometry.thumb.y&&y<geometry.thumb.y+geometry.thumb.height){dragNode=box.node;dragOffset=y-geometry.thumb.y;return true;}
        const float old=box.node->scrollTop;if(y<geometry.trackStart)box.node->scrollTop=std::max(0.0f,old-90.0f);else if(y>=geometry.track.y+geometry.track.height-geometry.arrowHeight)box.node->scrollTop=std::min(geometry.maximum,old+90.0f);else if(y<geometry.thumb.y)box.node->scrollTop=std::max(0.0f,old-box.content.height*0.9f);else box.node->scrollTop=std::min(geometry.maximum,old+box.content.height*0.9f);ApplyScrollOffset(box,old,viewportHeight_);return true;
    }
    for(auto it=box.children.rbegin();it!=box.children.rend();++it)if(BeginScrollbarBox(**it,x,y,dragNode,dragOffset))return true;return false;
}
bool LayoutEngine::DragScrollbar(const std::shared_ptr<Node>& node,float y,float dragOffset){
    if(!root_||!node)return false;const auto found=boxIndex_.find(node.get());auto* box=found==boxIndex_.end()?nullptr:found->second;VerticalScrollbarGeometry geometry;if(!box||!VerticalScrollbarFor(*box,styleSheet_,geometry)||geometry.travel<=0||geometry.maximum<=0)return false;const float thumbY=std::max(geometry.trackStart,std::min(geometry.trackStart+geometry.travel,y-dragOffset));const float value=(thumbY-geometry.trackStart)/geometry.travel*geometry.maximum;const float old=node->scrollTop;node->scrollTop=std::max(0.0f,std::min(geometry.maximum,value));ApplyScrollOffset(*box,old,viewportHeight_);return std::abs(node->scrollTop-old)>0.01f;
}

void LayoutEngine::DumpBox(const LayoutBox& box,std::wstring& output,bool& first)const{if(!box.visible)return;if(box.node->type==NodeType::Element){if(!first)output+=L",";first=false;std::wostringstream s;s<<L"{\"tag\":\""<<EscapeJson(box.node->tag)<<L"\",\"id\":\""<<EscapeJson(box.node->Attribute(L"id"))<<L"\",\"x\":"<<std::lround(box.rect.x)<<L",\"y\":"<<std::lround(box.rect.y)<<L",\"width\":"<<std::lround(box.rect.width)<<L",\"height\":"<<std::lround(box.rect.height)<<L"}";output+=s.str();}for(auto& c:box.children)DumpBox(*c,output,first);}
std::wstring LayoutEngine::DumpJson()const{std::wstring out=L"[";bool first=true;if(root_)DumpBox(*root_,out,first);return out+L"]";}

} // namespace TWebFrame::Internal
