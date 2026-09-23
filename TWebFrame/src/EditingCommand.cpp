#include "EditingCommand.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace TWebFrame::Internal {
namespace {

bool IsVoidHtmlElement(const std::wstring& tag){
    static constexpr const wchar_t* names[]={
        L"area",L"base",L"br",L"col",L"embed",L"hr",L"img",L"input",
        L"link",L"meta",L"param",L"source",L"track",L"wbr"};
    return std::find(std::begin(names),std::end(names),tag)!=std::end(names);
}

std::shared_ptr<Node> CloneDomNode(const std::shared_ptr<Node>& source,bool deep){
    if(!source)return {};
    auto clone=std::make_shared<Node>();
    clone->type=source->type;clone->tag=source->tag;clone->text=source->text;
    clone->attributes=source->attributes;clone->inlineStyle=source->inlineStyle;
    clone->files=source->files;clone->checked=source->checked;
    clone->indeterminate=source->indeterminate;clone->disabled=source->disabled;
    clone->scrollLeft=source->scrollLeft;clone->scrollTop=source->scrollTop;
    clone->selectionStart=source->selectionStart;
    clone->selectionEnd=source->selectionEnd;
    clone->selectionDirection=source->selectionDirection;
    if(deep)for(const auto& child:source->children){
        auto copied=CloneDomNode(child,true);copied->parent=clone;
        clone->children.push_back(std::move(copied));
    }
    return clone;
}

} // namespace

struct EditingCommandExecutor::Impl {
    Document& document;
    SelectionProvider domSelectionProvider;
    SelectionSetter domSelectionSetter;
    MutationSink mutationSink;
    ConnectedNodeSink connectedNodeSink;

    Impl(Document& value,
         SelectionProvider provider,
         SelectionSetter setter,
         MutationSink mutations,
         ConnectedNodeSink connected)
        :document(value),
         domSelectionProvider(std::move(provider)),
         domSelectionSetter(std::move(setter)),
         mutationSink(std::move(mutations)),
         connectedNodeSink(std::move(connected)){}

    void Mutated(const std::shared_ptr<Node>& target,EditingMutationKind kind,
                 bool requiresIndex=false){
        if(mutationSink)mutationSink(target,kind,requiresIndex);
    }
    void ExecuteConnectedScripts(const std::shared_ptr<Node>& node){
        if(connectedNodeSink)connectedNodeSink(node);
    }
    static std::shared_ptr<Node> EditableRoot(std::shared_ptr<Node> node){
        std::shared_ptr<Node> result;
        for(auto current=node;current;current=current->parent.lock()){
            const auto editable=ToLower(Trim(current->Attribute(L"contenteditable")));
            if(current->attributes.count(L"contenteditable")&&editable!=L"false")result=current;
            else if(current->attributes.count(L"contenteditable")&&editable==L"false")break;
        }
        return result;
    }
    static bool IsEditingBlock(const std::wstring& tag){
        return tag==L"address"||tag==L"blockquote"||tag==L"div"||tag==L"p"||tag==L"pre"||
               tag==L"h1"||tag==L"h2"||tag==L"h3"||tag==L"h4"||tag==L"h5"||tag==L"h6";
    }
    bool QueryEditingCommandState(const std::wstring& rawCommand){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||!selection.anchorNode)return false;
        const auto command=ToLower(rawCommand);
        for(auto current=selection.anchorNode;current;current=current->parent.lock()){
            if(command==L"bold"&&(current->tag==L"b"||current->tag==L"strong"))return true;
            if(command==L"italic"&&(current->tag==L"i"||current->tag==L"em"))return true;
            if(command==L"strikethrough"&&(current->tag==L"s"||current->tag==L"strike"||current->tag==L"del"))return true;
            if(command==L"insertunorderedlist"&&current->tag==L"ul")return true;
            if(command==L"insertorderedlist"&&current->tag==L"ol")return true;
            if(EditableRoot(current)==current)break;
        }
        return false;
    }
    static bool IsInsertedBlock(const std::shared_ptr<Node>& node){
        if(!node||node->type!=NodeType::Element)return false;
        static constexpr const wchar_t* tags[]={
            L"address",L"article",L"aside",L"blockquote",L"div",L"dl",L"fieldset",
            L"footer",L"form",L"h1",L"h2",L"h3",L"h4",L"h5",L"h6",L"header",
            L"hgroup",L"hr",L"main",L"menu",L"nav",L"ol",L"p",L"pre",L"section",
            L"table",L"ul"};
        return std::find(std::begin(tags),std::end(tags),node->tag)!=std::end(tags);
    }
    static std::shared_ptr<Node> EditingTextNode(const std::wstring& text,
                                                 const std::shared_ptr<Node>& parent={}){
        auto node=std::make_shared<Node>();node->type=NodeType::Text;node->tag=L"#text";
        node->text=text;node->parent=parent;return node;
    }
    static std::shared_ptr<Node> LastEditingText(const std::shared_ptr<Node>& node){
        if(!node)return {};
        if(node->type==NodeType::Text)return node;
        for(auto child=node->children.rbegin();child!=node->children.rend();++child)
            if(auto text=LastEditingText(*child))return text;
        return {};
    }
    static std::shared_ptr<Node> EnsureInsertedCaretText(
        const std::vector<std::shared_ptr<Node>>& inserted){
        if(inserted.empty())return {};
        auto last=inserted.back();
        if(auto text=LastEditingText(last))return text;
        if(!last||last->type!=NodeType::Element||IsVoidHtmlElement(last->tag))return {};
        // A trailing <p><br></p> is the conventional editable caret block.
        // Back the caret with one persistent empty text node so DOM normalize()
        // cannot detach the native editing selection immediately after insertion.
        if(last->children.size()==1&&last->children.front()->type==NodeType::Element&&
           last->children.front()->tag==L"br"){
            last->children.front()->parent.reset();last->children.clear();
        }
        auto text=EditingTextNode(L"",last);last->children.push_back(text);return text;
    }
    bool InsertEditingHtml(const std::wstring& html){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||!selection.focusNode)return false;
        auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;
        auto fragment=document.ParseFragment(html);
        if(fragment.empty()&&!html.empty())return false;
        bool indexOk=true;
        std::shared_ptr<Node> caretText;
        bool caretBeforeText=false;
        auto attach=[&](const std::shared_ptr<Node>& parent,size_t position,
                        std::vector<std::shared_ptr<Node>>& nodes){
            position=std::min(position,parent->children.size());
            for(auto& node:nodes)node->parent=parent;
            parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(position),
                                    nodes.begin(),nodes.end());
            for(const auto& node:nodes){
                indexOk=document.IndexSubtree(node)&&indexOk;
                ExecuteConnectedScripts(node);
            }
        };

        if(selection.anchorNode==selection.focusNode&&
           selection.anchorNode->type==NodeType::Text){
            const auto textNode=selection.anchorNode;
            const auto parent=textNode->parent.lock();if(!parent)return false;
            const size_t start=std::min({selection.anchorOffset,selection.focusOffset,
                                         textNode->text.size()});
            const size_t end=std::min(std::max(selection.anchorOffset,selection.focusOffset),
                                      textNode->text.size());
            const auto before=textNode->text.substr(0,start),after=textNode->text.substr(end);
            const bool insertsBlock=std::any_of(fragment.begin(),fragment.end(),IsInsertedBlock);
            auto directBlock=parent;
            while(directBlock&&directBlock->parent.lock()!=root)
                directBlock=directBlock->parent.lock();
            const bool splitSimpleBlock=insertsBlock&&directBlock&&
                directBlock->parent.lock()==root&&directBlock->children.size()==1&&
                directBlock->children.front()==textNode;
            if(splitSimpleBlock){
                const auto position=std::find(root->children.begin(),root->children.end(),directBlock);
                if(position==root->children.end())return false;
                const size_t offset=static_cast<size_t>(position-root->children.begin());
                indexOk=document.UnindexSubtree(directBlock)&&indexOk;
                root->children.erase(position);directBlock->parent.reset();
                std::vector<std::shared_ptr<Node>> inserted;
                if(!before.empty()){
                    textNode->text=before;textNode->parent=directBlock;directBlock->children={textNode};
                    inserted.push_back(directBlock);
                }
                inserted.insert(inserted.end(),fragment.begin(),fragment.end());
                if(!after.empty()){
                    auto trailing=CloneDomNode(directBlock,false);trailing->RemoveAttribute(L"id");
                    auto trailingText=EditingTextNode(after,trailing);trailing->children={trailingText};
                    inserted.push_back(trailing);caretText=trailingText;caretBeforeText=true;
                }
                if(!caretText)caretText=EnsureInsertedCaretText(inserted);
                attach(root,offset,inserted);
                if(!caretText){selection.anchorNode=selection.focusNode=root;
                    selection.anchorOffset=selection.focusOffset=offset+inserted.size();}
            }else{
                const auto position=std::find(parent->children.begin(),parent->children.end(),textNode);
                if(position==parent->children.end())return false;
                const size_t offset=static_cast<size_t>(position-parent->children.begin());
                indexOk=document.UnindexSubtree(textNode)&&indexOk;
                parent->children.erase(position);textNode->parent.reset();
                std::vector<std::shared_ptr<Node>> inserted;
                if(!before.empty())inserted.push_back(EditingTextNode(before));
                inserted.insert(inserted.end(),fragment.begin(),fragment.end());
                if(!after.empty()){
                    caretText=EditingTextNode(after);inserted.push_back(caretText);caretBeforeText=true;
                }
                if(!caretText)caretText=EnsureInsertedCaretText(inserted);
                attach(parent,offset,inserted);
                if(!caretText){selection.anchorNode=selection.focusNode=parent;
                    selection.anchorOffset=selection.focusOffset=offset+inserted.size();}
            }
        }else if(selection.anchorNode==selection.focusNode&&
                 selection.anchorNode->type!=NodeType::Text){
            const auto parent=selection.anchorNode;
            if(EditableRoot(parent)!=root&&parent!=root)return false;
            const size_t start=std::min({selection.anchorOffset,selection.focusOffset,
                                         parent->children.size()});
            const size_t end=std::min(std::max(selection.anchorOffset,selection.focusOffset),
                                      parent->children.size());
            for(size_t index=start;index<end;++index)
                indexOk=document.UnindexSubtree(parent->children[index])&&indexOk;
            for(size_t index=start;index<end;++index)parent->children[index]->parent.reset();
            parent->children.erase(parent->children.begin()+static_cast<std::ptrdiff_t>(start),
                                   parent->children.begin()+static_cast<std::ptrdiff_t>(end));
            caretText=EnsureInsertedCaretText(fragment);attach(parent,start,fragment);
            if(!caretText){selection.anchorNode=selection.focusNode=parent;
                selection.anchorOffset=selection.focusOffset=start+fragment.size();}
        }else return false;

        if(caretText){selection.anchorNode=selection.focusNode=caretText;
            selection.anchorOffset=selection.focusOffset=caretBeforeText?0:caretText->text.size();}
        Mutated(root,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    static bool IsInlineFormattingNode(const std::shared_ptr<Node>& node,
                                       const std::wstring& command){
        if(!node||node->type!=NodeType::Element)return false;
        if(command==L"bold")return node->tag==L"b"||node->tag==L"strong";
        if(command==L"italic")return node->tag==L"i"||node->tag==L"em";
        if(command==L"strikethrough")
            return node->tag==L"s"||node->tag==L"strike"||node->tag==L"del";
        return false;
    }
    bool ExecuteInlineFormattingCommand(const std::wstring& command){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||selection.anchorNode!=selection.focusNode||
           selection.anchorNode->type!=NodeType::Text)return false;
        const auto textNode=selection.anchorNode;
        const auto root=EditableRoot(textNode);if(!root)return false;
        const auto parent=textNode->parent.lock();if(!parent)return false;
        const size_t start=std::min({selection.anchorOffset,selection.focusOffset,
                                     textNode->text.size()});
        const size_t end=std::min(std::max(selection.anchorOffset,selection.focusOffset),
                                  textNode->text.size());
        if(start==end)return false;
        const bool backward=selection.anchorOffset>selection.focusOffset;
        const auto selectedValue=textNode->text.substr(start,end-start);
        const auto beforeValue=textNode->text.substr(0,start);
        const auto afterValue=textNode->text.substr(end);
        const auto makeText=[&](const std::wstring& value,
                                const std::shared_ptr<Node>& owner){
            auto text=EditingTextNode(value,owner);
            text->ownerDocument=textNode->ownerDocument;return text;
        };

        std::shared_ptr<Node> formatting;
        for(auto current=parent;current&&current!=root;current=current->parent.lock())
            if(IsInlineFormattingNode(current,command)){formatting=current;break;}

        if(formatting&&formatting==parent&&formatting->children.size()==1){
            // Toggle a simple formatted run off while preserving formatting on
            // any unselected prefix and suffix of the same text node.
            const auto container=formatting->parent.lock();if(!container)return false;
            const auto position=std::find(container->children.begin(),container->children.end(),formatting);
            if(position==container->children.end())return false;
            std::vector<std::shared_ptr<Node>> replacement;
            if(!beforeValue.empty()){
                auto before=makeText(beforeValue,formatting);
                formatting->children={before};replacement.push_back(formatting);
            }
            textNode->text=selectedValue;textNode->parent=container;
            replacement.push_back(textNode);
            if(!afterValue.empty()){
                auto afterFormatting=beforeValue.empty()?formatting:CloneDomNode(formatting,false);
                afterFormatting->ownerDocument=formatting->ownerDocument;
                if(!beforeValue.empty())afterFormatting->RemoveAttribute(L"id");
                auto after=makeText(afterValue,afterFormatting);
                afterFormatting->children={after};afterFormatting->parent=container;
                replacement.push_back(afterFormatting);
            }else if(beforeValue.empty())formatting->parent.reset();
            const auto index=static_cast<size_t>(position-container->children.begin());
            container->children.erase(position);
            container->children.insert(container->children.begin()+static_cast<std::ptrdiff_t>(index),
                                       replacement.begin(),replacement.end());
            for(auto& node:replacement)node->parent=container;
            Mutated(container,EditingMutationKind::Tree,true);
        }else{
            const auto position=std::find(parent->children.begin(),parent->children.end(),textNode);
            if(position==parent->children.end())return false;
            const wchar_t* tag=command==L"bold"?L"strong":
                               command==L"italic"?L"em":L"s";
            auto wrapper=document.CreateElement(tag);wrapper->parent=parent;
            textNode->text=selectedValue;textNode->parent=wrapper;
            wrapper->children={textNode};
            std::vector<std::shared_ptr<Node>> replacement;
            if(!beforeValue.empty())replacement.push_back(makeText(beforeValue,parent));
            replacement.push_back(wrapper);
            if(!afterValue.empty())replacement.push_back(makeText(afterValue,parent));
            const auto index=static_cast<size_t>(position-parent->children.begin());
            parent->children.erase(position);
            parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(index),
                                    replacement.begin(),replacement.end());
            Mutated(parent,EditingMutationKind::Tree,true);
        }
        selection.anchorNode=selection.focusNode=textNode;
        selection.anchorOffset=backward?selectedValue.size():0;
        selection.focusOffset=backward?0:selectedValue.size();
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    static std::shared_ptr<Node> EditingAnchor(const std::shared_ptr<Node>& node,
                                               const std::shared_ptr<Node>& root){
        for(auto current=node;current&&current!=root;current=current->parent.lock())
            if(current->type==NodeType::Element&&current->tag==L"a")return current;
        return {};
    }
    bool ExecuteCreateLinkCommand(const std::wstring& href){
        if(href.empty())return false;
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||selection.anchorNode!=selection.focusNode)return false;
        const auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;
        if(selection.anchorNode->type!=NodeType::Text){
            const auto container=selection.anchorNode;
            if(container->type!=NodeType::Element||
               selection.anchorOffset!=selection.focusOffset)return false;
            size_t position=std::min(selection.anchorOffset,container->children.size());
            bool indexOk=true;
            if(container->children.size()==1&&container->children.front()->tag==L"br"){
                const auto placeholder=container->children.front();
                indexOk=document.UnindexSubtree(placeholder)&&indexOk;
                placeholder->parent.reset();container->children.clear();position=0;
            }
            auto createdAnchor=document.CreateElement(L"a");
            createdAnchor->SetAttribute(L"href",href);createdAnchor->parent=container;
            auto textNode=EditingTextNode(href,createdAnchor);
            textNode->ownerDocument=container->ownerDocument;
            createdAnchor->children={textNode};
            container->children.insert(
                container->children.begin()+static_cast<std::ptrdiff_t>(position),
                createdAnchor);
            indexOk=document.IndexSubtree(createdAnchor)&&indexOk;
            Mutated(container,EditingMutationKind::Tree,!indexOk);
            selection.anchorNode=selection.focusNode=textNode;
            selection.anchorOffset=0;selection.focusOffset=href.size();
            if(domSelectionSetter)domSelectionSetter(selection);
            return true;
        }
        const auto textNode=selection.anchorNode;
        const auto parent=textNode->parent.lock();if(!parent)return false;
        const size_t start=std::min({selection.anchorOffset,selection.focusOffset,
                                     textNode->text.size()});
        const size_t end=std::min(std::max(selection.anchorOffset,selection.focusOffset),
                                  textNode->text.size());
        const bool backward=selection.anchorOffset>selection.focusOffset;

        if(const auto anchor=EditingAnchor(textNode,root)){
            anchor->SetAttribute(L"href",href);
            Mutated(anchor,EditingMutationKind::Style);
            selection.anchorOffset=std::min(selection.anchorOffset,textNode->text.size());
            selection.focusOffset=std::min(selection.focusOffset,textNode->text.size());
            if(domSelectionSetter)domSelectionSetter(selection);
            return true;
        }
        const auto position=std::find(parent->children.begin(),parent->children.end(),textNode);
        if(position==parent->children.end())return false;
        const auto before=textNode->text.substr(0,start);
        const auto selected=start==end?href:textNode->text.substr(start,end-start);
        const auto after=textNode->text.substr(end);
        const auto makeText=[&](const std::wstring& value,
                                const std::shared_ptr<Node>& owner){
            auto text=EditingTextNode(value,owner);
            text->ownerDocument=textNode->ownerDocument;return text;
        };
        auto createdAnchor=document.CreateElement(L"a");
        createdAnchor->SetAttribute(L"href",href);
        createdAnchor->parent=parent;textNode->text=selected;
        textNode->parent=createdAnchor;createdAnchor->children={textNode};
        std::vector<std::shared_ptr<Node>> replacement;
        if(!before.empty())replacement.push_back(makeText(before,parent));
        replacement.push_back(createdAnchor);
        if(!after.empty())replacement.push_back(makeText(after,parent));
        const auto index=static_cast<size_t>(position-parent->children.begin());
        parent->children.erase(position);
        parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(index),
                                replacement.begin(),replacement.end());
        Mutated(parent,EditingMutationKind::Tree,true);
        selection.anchorNode=selection.focusNode=textNode;
        selection.anchorOffset=backward?selected.size():0;
        selection.focusOffset=backward?0:selected.size();
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    bool ExecuteUnlinkCommand(){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||!selection.focusNode)return false;
        const auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;
        const auto anchor=EditingAnchor(selection.anchorNode,root);
        if(!anchor||EditingAnchor(selection.focusNode,root)!=anchor)return false;
        const auto parent=anchor->parent.lock();if(!parent)return false;
        const auto position=std::find(parent->children.begin(),parent->children.end(),anchor);
        if(position==parent->children.end())return false;
        const auto index=static_cast<size_t>(position-parent->children.begin());
        bool indexOk=document.UnindexSubtree(anchor);
        parent->children.erase(position);
        auto children=std::move(anchor->children);anchor->children.clear();anchor->parent.reset();
        for(auto& child:children)child->parent=parent;
        parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(index),
                                children.begin(),children.end());
        for(const auto& child:children)indexOk=document.IndexSubtree(child)&&indexOk;
        Mutated(parent,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    static std::shared_ptr<Node> DirectEditingChild(
        const std::shared_ptr<Node>& root,const std::shared_ptr<Node>& node,size_t offset){
        if(!root||!node)return {};
        if(node==root){
            const size_t index=std::min(offset,root->children.size());
            if(index<root->children.size())return root->children[index];
            return root->children.empty()?std::shared_ptr<Node>{}:root->children.back();
        }
        auto direct=node;
        while(direct&&direct->parent.lock()!=root)direct=direct->parent.lock();
        return direct;
    }
    static std::shared_ptr<Node> EditingList(
        const std::shared_ptr<Node>& root,const std::shared_ptr<Node>& node,size_t offset){
        for(auto current=node;current&&current!=root;current=current->parent.lock())
            if(current->type==NodeType::Element&&
               (current->tag==L"ul"||current->tag==L"ol"))return current;
        const auto direct=DirectEditingChild(root,node,offset);
        return direct&&direct->type==NodeType::Element&&
               (direct->tag==L"ul"||direct->tag==L"ol")?direct:std::shared_ptr<Node>{};
    }
    bool ExecuteListCommand(const std::wstring& tag){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||!selection.focusNode)return false;
        const auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;

        const auto anchorList=EditingList(root,selection.anchorNode,selection.anchorOffset);
        const auto focusList=EditingList(root,selection.focusNode,selection.focusOffset);
        if(anchorList&&anchorList==focusList){
            if(anchorList->tag!=tag){
                const bool indexOk=document.UnindexSubtree(anchorList);
                anchorList->tag=tag;
                const bool restored=document.IndexSubtree(anchorList);
                Mutated(anchorList,EditingMutationKind::Tree,
                        !indexOk||!restored);
                if(domSelectionSetter)domSelectionSetter(selection);
                return true;
            }

            const auto parent=anchorList->parent.lock();if(!parent)return false;
            const auto position=std::find(parent->children.begin(),parent->children.end(),anchorList);
            if(position==parent->children.end())return false;
            const size_t insertion=static_cast<size_t>(position-parent->children.begin());
            bool indexOk=document.UnindexSubtree(anchorList);
            parent->children.erase(position);
            std::vector<std::shared_ptr<Node>> blocks;
            for(auto& child:anchorList->children){
                if(!child||child->tag!=L"li")continue;
                auto block=document.CreateElement(L"div");block->parent=parent;
                block->children=std::move(child->children);
                child->children.clear();child->parent.reset();
                for(auto& item:block->children)item->parent=block;
                if(block->children.empty()){
                    auto text=EditingTextNode(L"",block);
                    text->ownerDocument=block->ownerDocument;block->children.push_back(text);
                }
                const auto remap=[&](std::shared_ptr<Node>& node,size_t& offset){
                    if(node==child){node=block;offset=std::min(offset,block->children.size());}
                };
                remap(selection.anchorNode,selection.anchorOffset);
                remap(selection.focusNode,selection.focusOffset);
                blocks.push_back(std::move(block));
            }
            anchorList->children.clear();anchorList->parent.reset();
            const auto remapList=[&](std::shared_ptr<Node>& node,size_t& offset){
                if(node!=anchorList)return;
                node=parent;offset=insertion+std::min(offset,blocks.size());
            };
            remapList(selection.anchorNode,selection.anchorOffset);
            remapList(selection.focusNode,selection.focusOffset);
            parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insertion),
                                    blocks.begin(),blocks.end());
            for(const auto& block:blocks)indexOk=document.IndexSubtree(block)&&indexOk;
            Mutated(parent,EditingMutationKind::Tree,!indexOk);
            if(domSelectionSetter)domSelectionSetter(selection);
            return true;
        }

        auto anchor=DirectEditingChild(root,selection.anchorNode,selection.anchorOffset);
        auto focus=DirectEditingChild(root,selection.focusNode,selection.focusOffset);
        size_t first=0,last=0;
        if(!root->children.empty()){
            if(!anchor||!focus)return false;
            const auto anchorPosition=std::find(root->children.begin(),root->children.end(),anchor);
            const auto focusPosition=std::find(root->children.begin(),root->children.end(),focus);
            if(anchorPosition==root->children.end()||focusPosition==root->children.end())return false;
            first=static_cast<size_t>(std::min(anchorPosition,focusPosition)-root->children.begin());
            last=static_cast<size_t>(std::max(anchorPosition,focusPosition)-root->children.begin());
        }

        auto list=document.CreateElement(tag);list->parent=root;
        std::vector<std::shared_ptr<Node>> items;
        bool indexOk=true;
        if(root->children.empty()){
            auto item=document.CreateElement(L"li");item->parent=list;
            auto text=EditingTextNode(L"",item);text->ownerDocument=item->ownerDocument;
            item->children.push_back(text);items.push_back(item);
            selection.anchorNode=selection.focusNode=text;
            selection.anchorOffset=selection.focusOffset=0;
        }else{
            std::vector<std::shared_ptr<Node>> selected(
                root->children.begin()+static_cast<std::ptrdiff_t>(first),
                root->children.begin()+static_cast<std::ptrdiff_t>(last+1));
            for(const auto& child:selected)indexOk=document.UnindexSubtree(child)&&indexOk;
            root->children.erase(root->children.begin()+static_cast<std::ptrdiff_t>(first),
                                 root->children.begin()+static_cast<std::ptrdiff_t>(last+1));
            for(auto& child:selected){
                auto item=document.CreateElement(L"li");item->parent=list;
                const bool flatten=child&&child->type==NodeType::Element&&
                    IsEditingBlock(child->tag)&&child->tag!=L"blockquote"&&child->tag!=L"pre";
                if(flatten){
                    item->children=std::move(child->children);child->children.clear();
                    const auto remap=[&](std::shared_ptr<Node>& node,size_t& offset){
                        if(node==child){node=item;offset=std::min(offset,item->children.size());}
                    };
                    remap(selection.anchorNode,selection.anchorOffset);
                    remap(selection.focusNode,selection.focusOffset);
                    child->parent.reset();
                    for(auto& content:item->children)content->parent=item;
                }else if(child&&child->tag==L"br"){
                    child->parent.reset();
                }else if(child){
                    child->parent=item;item->children.push_back(child);
                }
                if(item->children.size()==1&&item->children.front()->tag==L"br"){
                    item->children.front()->parent.reset();item->children.clear();
                }
                if(item->children.empty()){
                    auto text=EditingTextNode(L"",item);text->ownerDocument=item->ownerDocument;
                    item->children.push_back(text);
                    if(selection.anchorNode==child){selection.anchorNode=text;selection.anchorOffset=0;}
                    if(selection.focusNode==child){selection.focusNode=text;selection.focusOffset=0;}
                }
                items.push_back(std::move(item));
            }
            const auto remapRoot=[&](std::shared_ptr<Node>& node,size_t& offset){
                if(node!=root||items.empty())return;
                const bool atEnd=offset>last;
                node=atEnd?items.back():items.front();
                offset=atEnd?node->children.size():0;
            };
            remapRoot(selection.anchorNode,selection.anchorOffset);
            remapRoot(selection.focusNode,selection.focusOffset);
        }
        list->children=std::move(items);
        root->children.insert(root->children.begin()+static_cast<std::ptrdiff_t>(first),list);
        indexOk=document.IndexSubtree(list)&&indexOk;
        Mutated(root,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    bool ExecuteIndentCommand(){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||!selection.focusNode)return false;
        const auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;

        std::vector<std::shared_ptr<Node>> contents;
        size_t first=0,last=0;
        bool indexOk=true;
        if(root->children.empty()){
            auto paragraph=document.CreateElement(L"p");
            auto text=EditingTextNode(L"",paragraph);paragraph->children={text};
            contents.push_back(paragraph);
            selection.anchorNode=selection.focusNode=text;
            selection.anchorOffset=selection.focusOffset=0;
        }else{
            const auto anchor=DirectEditingChild(root,selection.anchorNode,selection.anchorOffset);
            const auto focus=DirectEditingChild(root,selection.focusNode,selection.focusOffset);
            if(!anchor||!focus)return false;
            const auto anchorPosition=std::find(root->children.begin(),root->children.end(),anchor);
            const auto focusPosition=std::find(root->children.begin(),root->children.end(),focus);
            if(anchorPosition==root->children.end()||focusPosition==root->children.end())return false;
            first=static_cast<size_t>(std::min(anchorPosition,focusPosition)-root->children.begin());
            last=static_cast<size_t>(std::max(anchorPosition,focusPosition)-root->children.begin());
            contents.assign(root->children.begin()+static_cast<std::ptrdiff_t>(first),
                            root->children.begin()+static_cast<std::ptrdiff_t>(last+1));
            for(const auto& child:contents)indexOk=document.UnindexSubtree(child)&&indexOk;
            root->children.erase(root->children.begin()+static_cast<std::ptrdiff_t>(first),
                                 root->children.begin()+static_cast<std::ptrdiff_t>(last+1));
        }

        auto quote=document.CreateElement(L"blockquote");
        quote->SetAttribute(L"style",L"margin: 0 0 0 40px; border: none; padding: 0px;");
        quote->parent=root;
        quote->children=contents;
        for(auto& child:quote->children)child->parent=quote;
        root->children.insert(root->children.begin()+static_cast<std::ptrdiff_t>(first),quote);
        indexOk=document.IndexSubtree(quote)&&indexOk;
        const auto remapRootBoundary=[&](std::shared_ptr<Node>& node,size_t& offset){
            if(node!=root)return;
            node=quote;
            offset=offset<=first?0:std::min(offset-first,quote->children.size());
        };
        remapRootBoundary(selection.anchorNode,selection.anchorOffset);
        remapRootBoundary(selection.focusNode,selection.focusOffset);
        Mutated(root,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    bool ExecuteOutdentCommand(){
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||
           !selection.anchorNode||!selection.focusNode)return false;
        const auto root=EditableRoot(selection.anchorNode);
        if(!root||EditableRoot(selection.focusNode)!=root)return false;
        const auto containingQuote=[&](const std::shared_ptr<Node>& node,size_t offset){
            for(auto current=node;current&&current!=root;current=current->parent.lock())
                if(current->type==NodeType::Element&&current->tag==L"blockquote")return current;
            auto direct=DirectEditingChild(root,node,offset);
            return direct&&direct->type==NodeType::Element&&direct->tag==L"blockquote"?
                   direct:std::shared_ptr<Node>{};
        };
        const auto quote=containingQuote(selection.anchorNode,selection.anchorOffset);
        if(!quote||containingQuote(selection.focusNode,selection.focusOffset)!=quote)return false;
        const auto parent=quote->parent.lock();if(!parent)return false;
        const auto position=std::find(parent->children.begin(),parent->children.end(),quote);
        if(position==parent->children.end())return false;
        const size_t index=static_cast<size_t>(position-parent->children.begin());
        bool indexOk=document.UnindexSubtree(quote);
        parent->children.erase(position);
        auto children=std::move(quote->children);quote->children.clear();quote->parent.reset();
        const auto remapQuoteBoundary=[&](std::shared_ptr<Node>& node,size_t& offset){
            if(node!=quote)return;
            node=parent;offset=index+std::min(offset,children.size());
        };
        remapQuoteBoundary(selection.anchorNode,selection.anchorOffset);
        remapQuoteBoundary(selection.focusNode,selection.focusOffset);
        for(auto& child:children)child->parent=parent;
        parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(index),
                                children.begin(),children.end());
        for(const auto& child:children)indexOk=document.IndexSubtree(child)&&indexOk;
        Mutated(parent,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
    bool ExecuteEditingCommand(const std::wstring& rawCommand,const std::wstring& rawValue){
        const auto command=ToLower(Trim(rawCommand));
        if(command==L"inserthtml")return InsertEditingHtml(rawValue);
        if(command==L"inserthorizontalrule")return InsertEditingHtml(L"<hr>");
        if(command==L"insertunorderedlist")return ExecuteListCommand(L"ul");
        if(command==L"insertorderedlist")return ExecuteListCommand(L"ol");
        if(command==L"indent")return ExecuteIndentCommand();
        if(command==L"outdent")return ExecuteOutdentCommand();
        if(command==L"bold"||command==L"italic"||command==L"strikethrough")
            return ExecuteInlineFormattingCommand(command);
        if(command==L"createlink")return ExecuteCreateLinkCommand(rawValue);
        if(command==L"unlink")return ExecuteUnlinkCommand();
        if(command!=L"formatblock")return false;
        EditingSelection selection;
        if(!domSelectionProvider||!domSelectionProvider(selection)||!selection.anchorNode)return false;
        auto root=EditableRoot(selection.anchorNode);if(!root)return false;
        auto tag=ToLower(Trim(rawValue));
        if(tag.size()>2&&tag.front()==L'<'&&tag.back()==L'>')tag=tag.substr(1,tag.size()-2);
        if(!IsEditingBlock(tag))return false;

        std::shared_ptr<Node> block;
        for(auto current=selection.anchorNode;current&&current!=root;current=current->parent.lock())
            if(IsEditingBlock(current->tag)){block=current;break;}
        bool indexOk=true;
        if(block){
            if(block->tag==tag)return true;
            indexOk=document.UnindexSubtree(block);block->tag=tag;
            indexOk=document.IndexSubtree(block)&&indexOk;
        }else{
            auto direct=selection.anchorNode;
            if(direct==root){
                const size_t index=std::min(selection.anchorOffset,root->children.size());
                if(index<root->children.size())direct=root->children[index];
                else if(!root->children.empty())direct=root->children.back();
                else{
                    direct=std::make_shared<Node>();direct->type=NodeType::Text;direct->tag=L"#text";
                    direct->parent=root;root->children.push_back(direct);
                    selection.anchorNode=selection.focusNode=direct;
                    selection.anchorOffset=selection.focusOffset=0;
                }
            }
            while(direct&&direct->parent.lock()!=root)direct=direct->parent.lock();
            if(!direct)return false;
            const auto position=std::find(root->children.begin(),root->children.end(),direct);
            if(position==root->children.end())return false;
            block=document.CreateElement(tag);block->parent=root;direct->parent=block;block->children.push_back(direct);
            *position=block;indexOk=document.IndexSubtree(block);
        }
        Mutated(root,EditingMutationKind::Tree,!indexOk);
        if(domSelectionSetter)domSelectionSetter(selection);
        return true;
    }
};

EditingCommandExecutor::EditingCommandExecutor(
    Document& document,
    SelectionProvider selectionProvider,
    SelectionSetter selectionSetter,
    MutationSink mutationSink,
    ConnectedNodeSink connectedNodeSink)
    :impl_(std::make_unique<Impl>(
         document,std::move(selectionProvider),std::move(selectionSetter),
         std::move(mutationSink),std::move(connectedNodeSink))){}

EditingCommandExecutor::~EditingCommandExecutor()=default;

bool EditingCommandExecutor::Execute(const std::wstring& command,
                                     const std::wstring& value){
    return impl_->ExecuteEditingCommand(command,value);
}

bool EditingCommandExecutor::QueryState(const std::wstring& command) const{
    return impl_->QueryEditingCommandState(command);
}

bool EditingCommandExecutor::IsSupported(const std::wstring& rawCommand){
    const auto command=ToLower(Trim(rawCommand));
    return command==L"formatblock"||command==L"inserthtml"||
           command==L"inserthorizontalrule"||command==L"indent"||
           command==L"outdent"||command==L"insertunorderedlist"||
           command==L"insertorderedlist"||command==L"bold"||
           command==L"italic"||command==L"strikethrough"||
           command==L"createlink"||command==L"unlink";
}

} // namespace TWebFrame::Internal
