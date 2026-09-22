#pragma once

#include "DOM.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TWebFrame::Internal {

struct CssDeclaration {
    std::wstring name;
    std::wstring value;
    bool important = false;
};

struct CssRule {
    std::wstring selector;
    std::wstring pseudo;
    std::vector<std::wstring> selectorParts;
    std::vector<CssDeclaration> declarations;
    int specificity = 0;
    int order = 0;
    float minViewportWidth = 0.0f;
    float maxViewportWidth = std::numeric_limits<float>::infinity();
    float minViewportHeight = 0.0f;
    float maxViewportHeight = std::numeric_limits<float>::infinity();
    bool mediaEnabled = true;
    bool hasCustomDeclarations = false;
};

struct ComputedStyle {
    using ValueMap = FastMap<std::wstring, std::wstring>;
    std::shared_ptr<ValueMap> values = std::make_shared<ValueMap>();
    // Used-value layout metadata. CSS declarations remain device independent,
    // while borders snap to the physical-pixel grid like browser engines do.
    float deviceScale = 1.0f;
    std::wstring Get(const std::wstring& name, const std::wstring& fallback = L"") const;
    bool Is(const std::wstring& name, const std::wstring& value) const;
};

struct SelectPopupPalette {
    unsigned int border = 0;
    unsigned int background = 0;
    unsigned int selectedBackground = 0;
    unsigned int color = 0;
    unsigned int selectedColor = 0;
    unsigned int disabledColor = 0;
};

SelectPopupPalette ResolveSelectPopupPalette(const ComputedStyle& style,
                                             unsigned int effectiveBackground);

class StyleSheet {
public:
    bool Parse(const std::wstring& css, std::wstring* error = nullptr);
    void SetViewport(float width, float height) noexcept;
    ComputedStyle Compute(const std::shared_ptr<Node>& node,
                          const ComputedStyle* parent = nullptr,
                          const std::wstring& pseudo = L"") const;
    const std::vector<CssRule>& Rules() const { return rules_; }
    bool UsesNthChild() const noexcept { return usesNthChild_; }
    bool UsesNthChildFor(const std::shared_ptr<Node>& node) const;
    bool AttributeAffectsStyle(const std::wstring& name) const;
    bool HasPseudoRules(std::wstring_view pseudo) const;
    bool HasPseudoRulesFor(const std::shared_ptr<Node>& node,std::wstring_view pseudo) const;
    bool HoverStateAffects(const std::shared_ptr<Node>& node) const;
    bool HoverRequiresBroadInvalidation() const noexcept { return hoverRequiresBroadInvalidation_; }
    bool MutationRequiresBroadInvalidation() const noexcept { return mutationRequiresBroadInvalidation_; }
    std::uint64_t Version() const noexcept { return version_; }

    static float Length(const std::wstring& value, float reference, float viewport,
                        float fallback = 0.0f, float fontSize = 16.0f);
    static unsigned int Color(const std::wstring& value, unsigned int fallback = 0xff000000u);

private:
    std::vector<const CssRule*> CandidateRules(const std::shared_ptr<Node>& node) const;
    std::wstring ResolveVariables(const std::wstring& value,
                                  const FastMap<std::wstring, std::wstring>& vars,
                                  int depth = 0) const;
    std::vector<CssRule> rules_;
    FastMap<std::wstring, std::vector<size_t>> ruleIndex_;
    std::vector<size_t> universalRuleIndexes_;
    FastMap<std::wstring, bool> selectorAttributes_;
    FastMap<std::wstring, bool> nthChildSubjects_;
    bool hasUniversalNthChild_ = false;
    FastMap<std::wstring, std::wstring> rootVariables_;
    std::vector<std::wstring> pseudoRules_;
    std::vector<size_t> hoverRuleIndexes_;
    bool hoverRequiresBroadInvalidation_ = false;
    bool mutationRequiresBroadInvalidation_ = false;
    bool usesNthChild_ = false;
    bool usesViewportFontSize_ = false;
    float viewportWidth_ = std::numeric_limits<float>::infinity();
    float viewportHeight_ = std::numeric_limits<float>::infinity();
    std::uint64_t version_ = 0;
};

} // namespace TWebFrame::Internal
