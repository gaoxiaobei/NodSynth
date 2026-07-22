#pragma once

#include <cstddef>
#include <map>

#include <nodsynth/model/NodeSchema.h>

namespace nodsynth::model {
class SchemaRegistry {
public:
    bool registerSchema(NodeSchema schema);
    [[nodiscard]] const NodeSchema* find(const NodeTypeId& typeId) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return schemas_.size(); }

private:
    std::map<NodeTypeId, NodeSchema> schemas_;
};
} // namespace nodsynth::model
