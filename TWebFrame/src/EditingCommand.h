#pragma once

#include "DOM.h"

#include <functional>
#include <memory>
#include <string>

namespace TWebFrame::Internal {

struct EditingSelection {
    std::shared_ptr<Node> anchorNode;
    size_t anchorOffset = 0;
    std::shared_ptr<Node> focusNode;
    size_t focusOffset = 0;
};

enum class EditingMutationKind { Style, Tree };

class EditingCommandExecutor {
public:
    using SelectionProvider = std::function<bool(EditingSelection&)>;
    using SelectionSetter = std::function<void(const EditingSelection&)>;
    using MutationSink = std::function<void(const std::shared_ptr<Node>&,
                                            EditingMutationKind, bool)>;
    using ConnectedNodeSink = std::function<void(const std::shared_ptr<Node>&)>;

    EditingCommandExecutor(Document& document,
                           SelectionProvider selectionProvider,
                           SelectionSetter selectionSetter,
                           MutationSink mutationSink,
                           ConnectedNodeSink connectedNodeSink);
    ~EditingCommandExecutor();

    EditingCommandExecutor(const EditingCommandExecutor&) = delete;
    EditingCommandExecutor& operator=(const EditingCommandExecutor&) = delete;

    bool Execute(const std::wstring& command, const std::wstring& value);
    bool QueryState(const std::wstring& command) const;
    static bool IsSupported(const std::wstring& command);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TWebFrame::Internal
