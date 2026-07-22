#include <nodsynth/model/SchemaRegistry.h>

#include <set>
#include <utility>

namespace nodsynth::model {
bool SchemaRegistry::registerSchema(NodeSchema schema) {
    if (schema.typeId.value.empty() || schema.schemaVersion == 0 || schema.sourceId.empty()) return false;

    std::set<PortId> portIds;
    for (const auto& port : schema.ports) {
        if (port.id.value.empty() || port.channels == 0 || !portIds.insert(port.id).second) return false;
    }

    std::set<ParameterId> parameterIds;
    for (const auto& parameter : schema.parameters) {
        if (parameter.id.value.empty() || parameter.minimum > parameter.defaultValue ||
            parameter.defaultValue > parameter.maximum || !parameterIds.insert(parameter.id).second) return false;
        if (parameter.scale == ParameterScale::logarithmic && parameter.minimum <= 0.0) return false;
    }

    return schemas_.emplace(schema.typeId, std::move(schema)).second;
}

const NodeSchema* SchemaRegistry::find(const NodeTypeId& typeId) const noexcept {
    const auto found = schemas_.find(typeId);
    return found == schemas_.end() ? nullptr : &found->second;
}
} // namespace nodsynth::model
