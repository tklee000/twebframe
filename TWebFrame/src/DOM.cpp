#include "DOM.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <sstream>

namespace TWebFrame::Internal {

namespace {

bool IsSpace(wchar_t c) { return std::iswspace(c) != 0; }

bool IsVoidTag(const std::wstring& tag) {
    static const wchar_t* tags[] = {
        L"area", L"base", L"br", L"col", L"embed", L"hr", L"img", L"input",
        L"link", L"meta", L"param", L"source", L"track", L"wbr"};
    for (const auto* candidate : tags) if (tag == candidate) return true;
    return false;
}

bool ClosesOpenParagraph(const std::wstring& tag) {
    static const wchar_t* tags[] = {
        L"address", L"article", L"aside", L"blockquote", L"div", L"dl",
        L"fieldset", L"footer", L"form", L"h1", L"h2", L"h3", L"h4",
        L"h5", L"h6", L"header", L"hgroup", L"hr", L"main", L"menu",
        L"nav", L"ol", L"p", L"pre", L"section", L"table", L"ul"};
    for (const auto* candidate : tags) if (tag == candidate) return true;
    return false;
}

void CloseOpenElement(std::vector<std::shared_ptr<Node>>& stack,
                      const std::wstring& tag) {
    size_t match = stack.size();
    for (size_t index = stack.size(); index > 1; --index) {
        if (stack[index - 1]->tag == tag) { match = index - 1; break; }
    }
    if (match == stack.size()) return;
    stack.resize(match);
}

void ParseStyleAttribute(const std::wstring& source,
                         FastMap<std::wstring, std::wstring>& out) {
    size_t start = 0;
    while (start < source.size()) {
        size_t end = source.find(L';', start);
        if (end == std::wstring::npos) end = source.size();
        const auto part = source.substr(start, end - start);
        const size_t colon = part.find(L':');
        if (colon != std::wstring::npos) {
            auto name = ToLower(Trim(part.substr(0, colon)));
            auto value = Trim(part.substr(colon + 1));
            if (!name.empty()) out[name] = value;
        }
        start = end + 1;
    }
}

bool MatchSimple(const std::shared_ptr<Node>& node, std::wstring selector) {
    if (!node || node->type != NodeType::Element) return false;
    selector = Trim(selector);
    if (selector.empty() || selector == L"*") return true;

    // Supported dynamic pseudo classes and relational/simple functional selectors.
    for (;;) {
        const auto hasPos = selector.find(L":has(");
        if (hasPos == std::wstring::npos) break;
        size_t close=hasPos+5;int depth=1;for(;close<selector.size()&&depth;++close){if(selector[close]==L'(')++depth;else if(selector[close]==L')')--depth;}
        if(depth!=0)return false;const auto relative=Trim(selector.substr(hasPos+5,close-hasPos-6));bool found=false;
        std::function<void(const std::shared_ptr<Node>&)> find=[&](const std::shared_ptr<Node>& current){if(found)return;for(const auto& child:current->children){if(child->type==NodeType::Element&&MatchSimple(child,relative)){found=true;return;}find(child);}};
        find(node);if(!found)return false;selector.erase(hasPos,close-hasPos);
    }
    for (;;) {
        const auto notPos = selector.find(L":not(");
        if (notPos == std::wstring::npos) break;
        const auto close = selector.find(L')', notPos + 5);
        if (close == std::wstring::npos) return false;
        if (MatchSimple(node, selector.substr(notPos + 5, close - notPos - 5))) return false;
        selector.erase(notPos, close - notPos + 1);
    }
    auto consumePseudo = [&](const wchar_t* text, bool state) {
        const std::wstring p(text);
        size_t pos;
        while ((pos = selector.find(p)) != std::wstring::npos) {
            if (!state) return false;
            selector.erase(pos, p.size());
        }
        return true;
    };
    auto isEdgeChild=[&](bool first){
        auto parent=node->parent.lock();if(!parent)return false;
        if(first){for(const auto& child:parent->children)if(child->type==NodeType::Element)return child==node;}
        else{for(auto it=parent->children.rbegin();it!=parent->children.rend();++it)if((*it)->type==NodeType::Element)return *it==node;}
        return false;
    };
    auto isEmpty=[&](){
        for(const auto& child:node->children)
            if(child->type==NodeType::Element||
               (child->type==NodeType::Text&&!child->text.empty()))return false;
        return true;
    };
    auto childIndex=[&](){
        auto parent=node->parent.lock();if(!parent)return 0;int index=0;
        for(const auto& child:parent->children)if(child->type==NodeType::Element){++index;if(child==node)return index;}
        return 0;
    };
    auto typeIndex=[&](){
        auto parent=node->parent.lock();if(!parent)return 0;int index=0;
        for(const auto& child:parent->children)if(child->type==NodeType::Element&&child->tag==node->tag){++index;if(child==node)return index;}
        return 0;
    };
    auto matchesNth=[](int index,std::wstring expression){
        expression=ToLower(expression);expression.erase(std::remove_if(expression.begin(),expression.end(),IsSpace),expression.end());
        if(index<=0||expression.empty())return false;if(expression==L"odd")return index%2==1;if(expression==L"even")return index%2==0;
        try{
            const auto n=expression.find(L'n');if(n==std::wstring::npos)return index==std::stoi(expression);
            const auto coefficient=expression.substr(0,n);const int a=coefficient.empty()||coefficient==L"+"?1:(coefficient==L"-"?-1:std::stoi(coefficient));
            const int b=n+1>=expression.size()?0:std::stoi(expression.substr(n+1));if(a==0)return index==b;
            const int delta=index-b;return delta%a==0&&delta/a>=0;
        }catch(...){return false;}
    };
    for(;;){
        const auto position=selector.find(L":nth-child(");if(position==std::wstring::npos)break;
        const auto close=selector.find(L')',position+11);if(close==std::wstring::npos||!matchesNth(childIndex(),selector.substr(position+11,close-position-11)))return false;
        selector.erase(position,close-position+1);
    }
    for(;;){
        const auto position=selector.find(L":nth-of-type(");if(position==std::wstring::npos)break;
        const auto close=selector.find(L')',position+13);if(close==std::wstring::npos||!matchesNth(typeIndex(),selector.substr(position+13,close-position-13)))return false;
        selector.erase(position,close-position+1);
    }
    // :root is a regular pseudo-class and may be compounded with an attribute,
    // class, or id selector (for example :root[data-theme="light"]).  Keeping
    // it in the token stream made the generic parser read "root" as a tag.
    for(;;){
        const auto position=selector.find(L":root");if(position==std::wstring::npos)break;
        const auto end=position+5;
        if((position>0&&(std::iswalnum(selector[position-1])||selector[position-1]==L'-'||selector[position-1]==L'_'))||
           (end<selector.size()&&(std::iswalnum(selector[end])||selector[end]==L'-'||selector[end]==L'_')))return false;
        if(node->tag!=L"html")return false;
        selector.erase(position,5);
    }
    if (!consumePseudo(L":checked", node->checked) ||
        !consumePseudo(L":disabled", node->disabled) ||
        !consumePseudo(L":hover", node->hovered) ||
        !consumePseudo(L":focus-within", node->focusWithin||node->focused) ||
        !consumePseudo(L":focus-visible", node->focusVisible) ||
        !consumePseudo(L":focus", node->focused) ||
        !consumePseudo(L":empty",isEmpty()) ||
        !consumePseudo(L":first-child",isEdgeChild(true)) ||
        !consumePseudo(L":last-child",isEdgeChild(false))) return false;
    if(selector.find(L":active")!=std::wstring::npos)return false;
    selector.erase(std::remove(selector.begin(), selector.end(), L':'), selector.end());

    size_t i = 0;
    if (i < selector.size() && (std::iswalpha(selector[i]) || selector[i] == L'_')) {
        const size_t begin = i++;
        while (i < selector.size() && (std::iswalnum(selector[i]) || selector[i] == L'-' || selector[i] == L'_')) ++i;
        if (node->tag != ToLower(selector.substr(begin, i - begin))) return false;
    }
    while (i < selector.size()) {
        if (selector[i] == L'#' || selector[i] == L'.') {
            const wchar_t kind = selector[i++];
            const size_t begin = i;
            while (i < selector.size() && (std::iswalnum(selector[i]) || selector[i] == L'-' || selector[i] == L'_')) ++i;
            const auto word = selector.substr(begin, i - begin);
            if (kind == L'#' && node->Attribute(L"id") != word) return false;
            if (kind == L'.' && !node->HasClass(word)) return false;
        } else if (selector[i] == L'[') {
            const size_t close = selector.find(L']', i + 1);
            if (close == std::wstring::npos) return false;
            const auto expression = Trim(selector.substr(i + 1, close - i - 1));
            const size_t equals=expression.find(L'=');
            size_t operatorPosition=equals;
            std::wstring attributeOperator;
            if(equals!=std::wstring::npos){
                if(equals>0&&std::wstring(L"~|^$*").find(expression[equals-1])!=std::wstring::npos){
                    operatorPosition=equals-1;attributeOperator=expression.substr(equals-1,2);
                }else attributeOperator=L"=";
            }
            if (operatorPosition == std::wstring::npos) {
                if (node->attributes.count(ToLower(expression)) == 0) return false;
            } else {
                auto key = ToLower(Trim(expression.substr(0, operatorPosition)));
                auto value = Trim(expression.substr(operatorPosition + attributeOperator.size()));
                bool caseInsensitive=false;
                if(value.size()>=2&&std::iswspace(value[value.size()-2])&&
                   (value.back()==L'i'||value.back()==L'I')){
                    caseInsensitive=true;value=Trim(value.substr(0,value.size()-1));
                }
                if (value.size() >= 2 &&
                    ((value.front() == L'\'' && value.back() == L'\'') ||
                     (value.front() == L'"' && value.back() == L'"')))
                    value = value.substr(1, value.size() - 2);
                if(node->attributes.count(key)==0)return false;
                auto actual=node->Attribute(key);
                if(caseInsensitive){actual=ToLower(actual);value=ToLower(value);}
                bool matches=false;
                if(attributeOperator==L"=")matches=actual==value;
                else if(attributeOperator==L"^=")matches=actual.rfind(value,0)==0;
                else if(attributeOperator==L"$=")matches=actual.size()>=value.size()&&
                    actual.compare(actual.size()-value.size(),value.size(),value)==0;
                else if(attributeOperator==L"*=")matches=actual.find(value)!=std::wstring::npos;
                else if(attributeOperator==L"~="){
                    std::wistringstream words(actual);std::wstring word;
                    while(words>>word)if(word==value){matches=true;break;}
                }else if(attributeOperator==L"|=")matches=actual==value||
                    (actual.size()>value.size()&&actual.rfind(value+L"-",0)==0);
                if(!matches)return false;
            }
            i = close + 1;
        } else {
            ++i; // Unknown pseudo/state is ignored so one rule cannot abort parsing.
        }
    }
    return true;
}

std::vector<std::wstring> SplitSelector(std::wstring selector) {
    std::vector<std::wstring> result;std::wstring current;int bracket=0;
    auto flush=[&](){auto value=Trim(current);if(!value.empty())result.push_back(value);current.clear();};
    for(wchar_t c:selector){
        if(c==L'['||c==L'(')++bracket;
        if(c==L']'||c==L')')--bracket;
        if(bracket==0&&(c==L'>'||c==L'+'||c==L'~')){flush();result.push_back(std::wstring(1,c));}
        else if(bracket==0&&IsSpace(c))flush();
        else current+=c;
    }
    flush();
    return result;
}

void Walk(const std::shared_ptr<Node>& node,
          const std::function<void(const std::shared_ptr<Node>&)>& fn) {
    if (!node) return;
    fn(node);
    for (const auto& child : node->children) Walk(child, fn);
}

class HtmlParser {
public:
    explicit HtmlParser(const std::wstring& html) : html_(html) {}

    std::shared_ptr<Node> Parse(bool fragment, std::wstring* error) {
        auto root = std::make_shared<Node>();
        root->type = fragment ? NodeType::Element : NodeType::Document;
        root->tag = fragment ? L"fragment" : L"#document";
        std::vector<std::shared_ptr<Node>> stack{root};

        while (position_ < html_.size()) {
            if (html_[position_] != L'<') {
                const size_t end = html_.find(L'<', position_);
                auto text = DecodeEntities(html_.substr(position_, end - position_));
                position_ = end == std::wstring::npos ? html_.size() : end;
                if (!text.empty()) {
                    auto node = std::make_shared<Node>();
                    node->type = NodeType::Text;
                    node->tag = L"#text";
                    node->text = text;
                    node->parent = stack.back();
                    stack.back()->children.push_back(node);
                }
                continue;
            }
            if (Starts(L"<!--")) {
                const size_t end = html_.find(L"-->", position_ + 4);
                position_ = end == std::wstring::npos ? html_.size() : end + 3;
                continue;
            }
            if (Starts(L"<!") || Starts(L"<?")) {
                const size_t end = html_.find(L'>', position_ + 2);
                position_ = end == std::wstring::npos ? html_.size() : end + 1;
                continue;
            }
            if (Starts(L"</")) {
                position_ += 2;
                const auto name = ReadName();
                const size_t end = html_.find(L'>', position_);
                position_ = end == std::wstring::npos ? html_.size() : end + 1;
                CloseOpenElement(stack, name);
                continue;
            }

            ++position_;
            const auto tag = ReadName();
            if (tag.empty()) { ++position_; continue; }
            // HTML permits the </p> end tag to be omitted. Starting another
            // paragraph or a block element implicitly closes the open one.
            if (ClosesOpenParagraph(tag)) CloseOpenElement(stack, L"p");
            auto node = std::make_shared<Node>();
            node->type = NodeType::Element;
            node->tag = tag;
            SkipSpace();
            bool selfClosing = false;
            while (position_ < html_.size() && html_[position_] != L'>') {
                if (html_[position_] == L'/' && position_ + 1 < html_.size() && html_[position_ + 1] == L'>') {
                    selfClosing = true;
                    position_ += 2;
                    break;
                }
                const auto key = ReadName();
                if (key.empty()) { ++position_; continue; }
                SkipSpace();
                std::wstring value;
                if (position_ < html_.size() && html_[position_] == L'=') {
                    ++position_; SkipSpace();
                    if (position_ < html_.size() && (html_[position_] == L'\'' || html_[position_] == L'"')) {
                        const wchar_t quote = html_[position_++];
                        const size_t end = html_.find(quote, position_);
                        value = html_.substr(position_, end - position_);
                        position_ = end == std::wstring::npos ? html_.size() : end + 1;
                    } else {
                        const size_t begin = position_;
                        while (position_ < html_.size() && !IsSpace(html_[position_]) && html_[position_] != L'>') ++position_;
                        value = html_.substr(begin, position_ - begin);
                    }
                }
                node->attributes[key] = DecodeEntities(value);
                SkipSpace();
            }
            if (position_ < html_.size() && html_[position_] == L'>') ++position_;
            node->checked = node->attributes.count(L"checked") != 0;
            node->disabled = node->attributes.count(L"disabled") != 0;
            const auto style = node->Attribute(L"style");
            if (!style.empty()) ParseStyleAttribute(style, node->inlineStyle);
            node->parent = stack.back();
            stack.back()->children.push_back(node);

            if ((tag == L"script" || tag == L"style") && !selfClosing) {
                const std::wstring closing = L"</" + tag;
                const size_t end = ToLower(html_).find(closing, position_);
                auto textNode = std::make_shared<Node>();
                textNode->type = NodeType::Text;
                textNode->tag = L"#text";
                textNode->text = html_.substr(position_, end - position_);
                textNode->parent = node;
                node->children.push_back(textNode);
                if (end == std::wstring::npos) position_ = html_.size();
                else {
                    const size_t close = html_.find(L'>', end + closing.size());
                    position_ = close == std::wstring::npos ? html_.size() : close + 1;
                }
            } else if (!selfClosing && !IsVoidTag(tag)) {
                stack.push_back(node);
            }
        }
        if (error) error->clear();
        return root;
    }

private:
    bool Starts(const wchar_t* value) const {
        const size_t count = std::wcslen(value);
        return position_ + count <= html_.size() && html_.compare(position_, count, value) == 0;
    }
    void SkipSpace() { while (position_ < html_.size() && IsSpace(html_[position_])) ++position_; }
    std::wstring ReadName() {
        SkipSpace();
        const size_t begin = position_;
        while (position_ < html_.size()) {
            const wchar_t c = html_[position_];
            if (!std::iswalnum(c) && c != L'-' && c != L'_' && c != L':') break;
            ++position_;
        }
        return ToLower(html_.substr(begin, position_ - begin));
    }
    const std::wstring& html_;
    size_t position_ = 0;
};

} // namespace

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

std::wstring Trim(const std::wstring& value) {
    size_t a = 0, b = value.size();
    while (a < b && IsSpace(value[a])) ++a;
    while (b > a && IsSpace(value[b - 1])) --b;
    return value.substr(a, b - a);
}

std::wstring DecodeEntities(const std::wstring& value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != L'&') { out += value[i]; continue; }
        const size_t end = value.find(L';', i + 1);
        if (end == std::wstring::npos || end - i > 12) { out += value[i]; continue; }
        const auto entity = value.substr(i + 1, end - i - 1);
        if (entity == L"lt") out += L'<';
        else if (entity == L"gt") out += L'>';
        else if (entity == L"amp") out += L'&';
        else if (entity == L"quot") out += L'"';
        else if (entity == L"apos" || entity == L"#39") out += L'\'';
        else if (entity == L"nbsp") out += static_cast<wchar_t>(0x00a0);
        else if (!entity.empty() && entity[0] == L'#') {
            try {
                const int base = entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X') ? 16 : 10;
                out += static_cast<wchar_t>(std::stoul(entity.substr(base == 16 ? 2 : 1), nullptr, base));
            } catch (...) { out += L'&' + entity + L';'; }
        } else { out += L'&' + entity + L';'; }
        i = end;
    }
    return out;
}

std::wstring Node::Attribute(const std::wstring& name) const {
    const auto it = attributes.find(ToLower(name));
    return it == attributes.end() ? L"" : it->second;
}

void Node::SetAttribute(const std::wstring& name, const std::wstring& value) {
    const auto key = ToLower(name);
    attributes[key] = value;
    if (key == L"checked") checked = true;
    if (key == L"disabled") disabled = true;
    if (key == L"style") { inlineStyle.clear(); ParseStyleAttribute(value, inlineStyle); }
}

void Node::RemoveAttribute(const std::wstring& name) {
    const auto key = ToLower(name);
    attributes.erase(key);
    if (key == L"checked") checked = false;
    if (key == L"disabled") disabled = false;
    if (key == L"style") inlineStyle.clear();
}

bool Node::HasClass(const std::wstring& name) const {
    std::wistringstream in(Attribute(L"class"));
    std::wstring item;
    while (in >> item) if (item == name) return true;
    return false;
}

void Node::AddClass(const std::wstring& name) {
    if (HasClass(name)) return;
    auto value = Attribute(L"class");
    if (!value.empty()) value += L' ';
    attributes[L"class"] = value + name;
}

void Node::RemoveClass(const std::wstring& name) {
    std::wistringstream in(Attribute(L"class"));
    std::wstring item, value;
    while (in >> item) if (item != name) { if (!value.empty()) value += L' '; value += item; }
    attributes[L"class"] = value;
}

void Node::ToggleClass(const std::wstring& name, bool force, bool hasForce) {
    const bool add = hasForce ? force : !HasClass(name);
    if (add) AddClass(name); else RemoveClass(name);
}

std::wstring Node::InnerText() const {
    if (type == NodeType::Text) return text;
    if (tag == L"br") return L"\n";
    std::wstring result;
    for (const auto& child : children) result += child->InnerText();
    return result;
}

void Node::SetInnerText(const std::wstring& value) {
    children.clear();
    auto child = std::make_shared<Node>();
    child->type = NodeType::Text; child->tag = L"#text"; child->text = value;
    child->parent = shared_from_this();
    children.push_back(child);
}

std::shared_ptr<Node> Node::Closest(const std::wstring& selector) {
    auto current = shared_from_this();
    while (current) {
        if (Document::MatchesSelector(current, selector)) return current;
        current = current->parent.lock();
    }
    return {};
}

Document::Document() {
    root_ = std::make_shared<Node>();
    root_->type = NodeType::Document; root_->tag = L"#document";
}

bool Document::Parse(const std::wstring& html, std::wstring* error) {
    HtmlParser parser(html);
    root_ = parser.Parse(false, error);
    Reindex();
    return root_ != nullptr;
}

std::vector<std::shared_ptr<Node>> Document::ParseFragment(const std::wstring& html,
                                                            std::wstring* error) {
    HtmlParser parser(html);
    auto fragment = parser.Parse(true, error);
    return fragment ? fragment->children : std::vector<std::shared_ptr<Node>>{};
}

std::shared_ptr<Node> Document::Body() const {
    return QuerySelector(L"body");
}

std::shared_ptr<Node> Document::GetElementById(const std::wstring& id) const {
    const auto it = ids_.find(id);
    return it == ids_.end() ? nullptr : it->second.lock();
}

std::vector<std::shared_ptr<Node>> Document::GetElementsByName(const std::wstring& name) const {
    std::vector<std::shared_ptr<Node>> result;
    Walk(root_, [&](const auto& node) { if (node->Attribute(L"name") == name) result.push_back(node); });
    return result;
}

bool Document::MatchesSelector(const std::shared_ptr<Node>& node, const std::wstring& selector) {
    return MatchesSelector(node,SplitSelector(selector));
}

std::vector<std::wstring> Document::CompileSelector(const std::wstring& selector) {
    return SplitSelector(selector);
}

bool Document::MatchesSelector(const std::shared_ptr<Node>& node,
                               const std::vector<std::wstring>& parts) {
    if (parts.empty()) return false;
    auto current=node;int index=static_cast<int>(parts.size())-1;
    if(parts[index]==L">"||parts[index]==L"+"||!MatchSimple(current,parts[index]))return false;
    --index;
    while(index>=0){
        bool direct=false,adjacent=false,generalSibling=false;
        if(parts[index]==L">"||parts[index]==L"+"||parts[index]==L"~"){
            direct=parts[index]==L">";adjacent=parts[index]==L"+";
            generalSibling=parts[index]==L"~";--index;if(index<0)return false;
        }
        if(adjacent||generalSibling){
            auto parent=current?current->parent.lock():nullptr;
            std::vector<std::shared_ptr<Node>> previous;
            if(parent)for(const auto& sibling:parent->children){
                if(sibling==current)break;
                if(sibling->type==NodeType::Element)previous.push_back(sibling);
            }
            current.reset();
            for(auto it=previous.rbegin();it!=previous.rend();++it)
                if(adjacent||MatchSimple(*it,parts[index])){current=*it;break;}
        }else current=current?current->parent.lock():nullptr;
        if(direct||adjacent){if(!current||!MatchSimple(current,parts[index]))return false;}
        else if(generalSibling){if(!current)return false;}
        else{while(current&&!MatchSimple(current,parts[index]))current=current->parent.lock();if(!current)return false;}
        --index;
    }
    return true;
}

std::shared_ptr<Node> Document::QuerySelector(const std::wstring& selector,
                                               const std::shared_ptr<Node>& scope) const {
    auto all = QuerySelectorAll(selector, scope);
    return all.empty() ? nullptr : all.front();
}

std::vector<std::shared_ptr<Node>> Document::QuerySelectorAll(
    const std::wstring& selector, const std::shared_ptr<Node>& scope) const {
    std::vector<std::shared_ptr<Node>> result;
    // Comma-separated selectors are supported outside attribute/pseudo brackets.
    std::vector<std::wstring> selectors;
    int nesting = 0; size_t start = 0;
    for (size_t i = 0; i <= selector.size(); ++i) {
        const wchar_t c = i < selector.size() ? selector[i] : L',';
        if (c == L'[' || c == L'(') ++nesting;
        if (c == L']' || c == L')') --nesting;
        if (c == L',' && nesting == 0) { selectors.push_back(Trim(selector.substr(start, i - start))); start = i + 1; }
    }
    Walk(scope ? scope : root_, [&](const auto& node) {
        for (const auto& item : selectors) {
            bool matched=false;
            if(scope&&item.rfind(L":scope",0)==0){
                auto remainder=Trim(item.substr(6));
                if(remainder.empty())matched=node==scope;
                else if(remainder[0]==L'>')matched=node->parent.lock()==scope&&MatchesSelector(node,Trim(remainder.substr(1)));
                else matched=node!=scope&&MatchesSelector(node,remainder);
            }else matched=MatchesSelector(node,item);
            if(matched){result.push_back(node);break;}
        }
    });
    return result;
}

std::shared_ptr<Node> Document::CreateElement(const std::wstring& tag) const {
    auto node = std::make_shared<Node>();
    node->type = NodeType::Element; node->tag = ToLower(tag);
    return node;
}

void Document::SetInnerHtml(const std::shared_ptr<Node>& node, const std::wstring& html,
                            bool reindex) {
    if (!node) return;
    auto children = ParseFragment(html);
    for (auto& child : children) child->parent = node;
    node->children = std::move(children);
    if (reindex) Reindex();
}

std::wstring Document::StyleText() const {
    std::wstring result;
    for (const auto& style : QuerySelectorAll(L"style")) result += style->InnerText() + L"\n";
    return result;
}

std::wstring Document::ScriptText() const {
    std::wstring result;
    for (const auto& script : QuerySelectorAll(L"script")) result += script->InnerText() + L"\n";
    return result;
}

void Document::Reindex() {
    ids_.clear();
    Walk(root_, [&](const auto& node) {
        const auto id = node->Attribute(L"id");
        if (!id.empty()) ids_[id] = node;
    });
}

} // namespace TWebFrame::Internal
