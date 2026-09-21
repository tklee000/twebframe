#pragma once

#include "FastMap.h"

#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

enum class NodeType { Document, Element, Text };

struct Node : std::enable_shared_from_this<Node> {
    struct FileInfo {
        std::wstring name;
        std::wstring type;
        std::wstring path;
        unsigned long long size = 0;
    };
    NodeType type = NodeType::Element;
    std::wstring tag;
    std::wstring text;
    FastMap<std::wstring, std::wstring> attributes;
    FastMap<std::wstring, std::wstring> inlineStyle;
    std::vector<std::shared_ptr<Node>> children;
    std::vector<FileInfo> files;
    std::weak_ptr<Node> parent;
    bool checked = false;
    bool disabled = false;
    bool hovered = false;
    bool focused = false;
    bool focusVisible = false;
    bool focusWithin = false;
    float scrollLeft = 0.0f;
    float scrollTop = 0.0f;
    size_t selectionStart = 0;
    size_t selectionEnd = 0;

    std::wstring Attribute(const std::wstring& name) const;
    void SetAttribute(const std::wstring& name, const std::wstring& value);
    void RemoveAttribute(const std::wstring& name);
    bool HasClass(const std::wstring& name) const;
    void AddClass(const std::wstring& name);
    void RemoveClass(const std::wstring& name);
    void ToggleClass(const std::wstring& name, bool force, bool hasForce);
    std::wstring InnerText() const;
    void SetInnerText(const std::wstring& value);
    std::shared_ptr<Node> Closest(const std::wstring& selector);
};

class Document {
public:
    Document();
    bool Parse(const std::wstring& html, std::wstring* error = nullptr);
    std::vector<std::shared_ptr<Node>> ParseFragment(const std::wstring& html,
                                                     std::wstring* error = nullptr);

    std::shared_ptr<Node> Root() const { return root_; }
    std::shared_ptr<Node> Body() const;
    std::shared_ptr<Node> GetElementById(const std::wstring& id) const;
    std::vector<std::shared_ptr<Node>> GetElementsByName(const std::wstring& name) const;
    std::shared_ptr<Node> QuerySelector(const std::wstring& selector,
                                        const std::shared_ptr<Node>& scope = {}) const;
    std::vector<std::shared_ptr<Node>> QuerySelectorAll(
        const std::wstring& selector, const std::shared_ptr<Node>& scope = {}) const;
    std::shared_ptr<Node> CreateElement(const std::wstring& tag) const;
    void SetInnerHtml(const std::shared_ptr<Node>& node, const std::wstring& html,
                      bool reindex = true);
    std::wstring StyleText() const;
    std::wstring ScriptText() const;
    void Reindex();

    static bool MatchesSelector(const std::shared_ptr<Node>& node,
                                const std::wstring& selector);
    static bool MatchesSelector(const std::shared_ptr<Node>& node,
                                const std::vector<std::wstring>& selectorParts);
    static std::vector<std::wstring> CompileSelector(const std::wstring& selector);

private:
    std::shared_ptr<Node> root_;
    FastMap<std::wstring, std::weak_ptr<Node>> ids_;
};

std::wstring ToLower(std::wstring value);
std::wstring Trim(const std::wstring& value);
std::wstring DecodeEntities(const std::wstring& value);

} // namespace TWebFrame::Internal
