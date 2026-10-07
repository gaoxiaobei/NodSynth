#include <nodsynth/model/SchemaRegistry.h>

#include <cmath>
#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace nodsynth::model {
namespace {
NodeScope resolveDomain(const NodeSchema& owner, const PortSchema& port) {
    switch (port.domain) {
        case PortDomain::perVoice: return NodeScope::perVoice;
        case PortDomain::global: return NodeScope::global;
        case PortDomain::sameAsNode: return owner.scope;
    }
    std::terminate();
}

bool hasValidDomain(const NodeSchema& owner, const PortSchema& port) {
    const auto domain = resolveDomain(owner, port);
    if (port.direction == PortDirection::output) return domain == owner.scope;
    if (domain == owner.scope) return true;
    return owner.scope == NodeScope::global && port.domain == PortDomain::perVoice;
}
} // namespace

bool SchemaRegistry::registerSchema(NodeSchema schema) {
    if (schema.typeId.value.empty() || schema.schemaVersion == 0 || schema.sourceId.empty()) return false;

    std::set<PortId> portIds;
    for (const auto& port : schema.ports) {
        if (port.id.value.empty() || port.channels == 0 || !hasValidDomain(schema, port) ||
            !portIds.insert(port.id).second) return false;
    }

    std::set<ParameterId> parameterIds;
    for (const auto& parameter : schema.parameters) {
        if (parameter.id.value.empty() || std::isnan(parameter.minimum) ||
            std::isnan(parameter.maximum) || !std::isfinite(parameter.defaultValue) ||
            parameter.minimum > parameter.defaultValue ||
            parameter.defaultValue > parameter.maximum || !parameterIds.insert(parameter.id).second) return false;
        if (parameter.scale == ParameterScale::logarithmic && parameter.minimum <= 0.0) return false;
        if (parameter.modulationMode != ModulationMode::replace) {
            if (!parameter.modulatable || parameter.modulationPort.empty()) return false;
            const auto input = std::find_if(schema.ports.begin(), schema.ports.end(), [&](const auto& port) {
                return port.id.value == parameter.modulationPort && port.direction == PortDirection::input && port.kind == PortKind::control;
            });
            if (input == schema.ports.end()) return false;
            if (parameter.modulationMode == ModulationMode::octave &&
                (parameter.minimum <= 0 || std::none_of(schema.parameters.begin(), schema.parameters.end(), [&](const auto& depth) {
                    return depth.id.value == parameter.depthParameter && depth.unit == "octaves";
                }))) return false;
        }
    }

    return schemas_.emplace(schema.typeId, std::move(schema)).second;
}

const NodeSchema* SchemaRegistry::find(const NodeTypeId& typeId) const noexcept {
    const auto found = schemas_.find(typeId);
    return found == schemas_.end() ? nullptr : &found->second;
}
} // namespace nodsynth::model
