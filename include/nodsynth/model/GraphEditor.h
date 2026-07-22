#pragma once

#include <functional>
#include <vector>

#include <nodsynth/model/GraphDocument.h>

namespace nodsynth::model {
class GraphEditor {
public:
    [[nodiscard]] const GraphDocument& document() const noexcept { return document_; }

    bool addNode(NodeRecord node);
    bool removeNode(const NodeId& id);
    bool connect(Connection connection);
    bool disconnect(Connection connection);
    bool setParameter(const NodeId& id, const ParameterId& parameterId, double value);
    bool undo();
    bool redo();

private:
    struct Edit {
        std::function<void(GraphDocument&)> apply;
        std::function<void(GraphDocument&)> revert;
    };

    void commit(Edit edit);

    GraphDocument document_;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
};
} // namespace nodsynth::model
