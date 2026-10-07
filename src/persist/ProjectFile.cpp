#include <nodsynth/persist/ProjectFile.h>

#include <fstream>
#include <system_error>
#include <utility>
#include <cmath>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nodsynth::persist {
namespace {
Json objectFromExtensions(const std::string& text) {
    std::string error;
    auto parsed = Json::parse(text, error);
    if (!parsed || parsed->kind() != Json::Kind::object) return Json::object();
    return std::move(*parsed);
}

bool knownNodeKey(std::string_view key) {
    return key == "id" || key == "typeId" || key == "schemaVersion" || key == "parameters" || key == "opaqueState" ||
           key == "position";
}

Json nodeJson(const model::NodeRecord& node) {
    auto object = objectFromExtensions(node.extensionsJson);
    object.set("id", Json::string(node.id.value));
    object.set("typeId", Json::string(node.typeId.value));
    object.set("schemaVersion", Json::number(node.schemaVersion));
    auto parameters = Json::object();
    for (const auto& [id, value] : node.parameters) parameters.set(id.value, Json::number(value));
    object.set("parameters", std::move(parameters));
    object.set("opaqueState", Json::string(node.opaqueStateJson.empty() ? "{}" : node.opaqueStateJson));
    auto position = Json::object();
    position.set("x", Json::number(node.position.x));
    position.set("y", Json::number(node.position.y));
    object.set("position", std::move(position));
    return object;
}

const Json* matchingConnection(const Json* previous, const model::Connection& connection) {
    if (previous == nullptr || previous->kind() != Json::Kind::array) return nullptr;
    for (const auto& item : previous->asArray()) {
        const auto* from = item.find("from");
        const auto* to = item.find("to");
        if (from == nullptr || to == nullptr) continue;
        const auto* fromNode = from->find("node");
        const auto* fromPort = from->find("port");
        const auto* toNode = to->find("node");
        const auto* toPort = to->find("port");
        if (fromNode == nullptr || fromPort == nullptr || toNode == nullptr || toPort == nullptr) continue;
        if (fromNode->asString() == connection.from.nodeId.value && fromPort->asString() == connection.from.portId.value &&
            toNode->asString() == connection.to.nodeId.value && toPort->asString() == connection.to.portId.value) {
            return &item;
        }
    }
    return nullptr;
}

Json endpointJson(const model::Endpoint& endpoint) {
    auto object = Json::object();
    object.set("node", Json::string(endpoint.nodeId.value));
    object.set("port", Json::string(endpoint.portId.value));
    return object;
}

Json connectionJson(const model::Connection& connection, const Json* previous) {
    auto object = previous != nullptr && previous->kind() == Json::Kind::object ? *previous : Json::object();
    object.set("from", endpointJson(connection.from));
    object.set("to", endpointJson(connection.to));
    return object;
}

Json buildRoot(const Json& previous, const model::Viewport& viewport, const model::GraphSnapshot& graph) {
    auto root = previous.kind() == Json::Kind::object ? previous : Json::object();
    root.set("format", Json::string("nodsynth.project"));
    root.set("formatVersion", Json::number(kProjectFormatVersion));
    auto view = Json::object();
    view.set("originX", Json::number(viewport.originX));
    view.set("originY", Json::number(viewport.originY));
    view.set("zoom", Json::number(viewport.zoom));
    root.set("viewport", std::move(view));
    auto nodes = Json::array();
    for (const auto& node : graph.nodes) nodes.push(nodeJson(node));
    root.set("nodes", std::move(nodes));
    const auto* previousConnections = previous.find("connections");
    auto connections = Json::array();
    for (const auto& connection : graph.connections) {
        connections.push(connectionJson(connection, matchingConnection(previousConnections, connection)));
    }
    root.set("connections", std::move(connections));
    auto macros=Json::array();
    for(const auto& macro:graph.macros) {
        auto entry=Json::object();entry.set("id",Json::string(macro.id));auto mappings=Json::array();
        for(const auto& mapping:macro.mappings) {
            auto item=Json::object();item.set("node",Json::string(mapping.node.value));item.set("parameter",Json::string(mapping.parameter.value));
            item.set("minimum",Json::number(mapping.minimum));item.set("maximum",Json::number(mapping.maximum));item.set("curve",Json::string(mapping.curve));mappings.push(std::move(item));
        }entry.set("mappings",std::move(mappings));macros.push(std::move(entry));
    }root.set("macros",std::move(macros));
    return root;
}

bool readText(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open the project file";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& target, std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "failed to replace the project file";
        return false;
    }
    return true;
#else
    std::error_code failure;
    std::filesystem::rename(temporary, target, failure);
    if (failure) {
        error = failure.message();
        return false;
    }
    return true;
#endif
}

std::optional<model::NodeRecord> readNode(const Json& json, std::string& error) {
    const auto* id = json.find("id");
    const auto* typeId = json.find("typeId");
    const auto* version = json.find("schemaVersion");
    if (id == nullptr || typeId == nullptr || version == nullptr || id->kind() != Json::Kind::string ||
        typeId->kind() != Json::Kind::string || version->kind() != Json::Kind::number) {
        error = "a project node is missing its identity";
        return std::nullopt;
    }
    model::NodeRecord record;
    record.id = model::NodeId{id->asString()};
    record.typeId = model::NodeTypeId{typeId->asString()};
    const double schemaVersion = version->asNumber();
    if (schemaVersion < 1.0 || schemaVersion > 4294967295.0) {
        error = "a project node has an unsupported schema version field";
        return std::nullopt;
    }
    record.schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    record.opaqueStateJson = "{}";
    if (const auto* opaque = json.find("opaqueState"); opaque != nullptr && opaque->kind() == Json::Kind::string) {
        record.opaqueStateJson = opaque->asString();
    }
    if (const auto* position = json.find("position")) {
        record.position.x = static_cast<float>(position->find("x") == nullptr ? 0.0 : position->find("x")->asNumber());
        record.position.y = static_cast<float>(position->find("y") == nullptr ? 0.0 : position->find("y")->asNumber());
    }
    if (const auto* parameters = json.find("parameters"); parameters != nullptr) {
        if (parameters->kind() != Json::Kind::object) {
            error = "node parameters must be a JSON object";
            return std::nullopt;
        }
        for (const auto& [name, value] : parameters->items()) {
            if (value.kind() != Json::Kind::number) {
                error = "node parameter '" + name + "' is not a number";
                return std::nullopt;
            }
            record.parameters.emplace(model::ParameterId{name}, value.asNumber());
        }
    }
    auto extras = Json::object();
    for (const auto& [key, value] : json.items()) {
        if (!knownNodeKey(key)) extras.set(key, value);
    }
    record.extensionsJson = extras.dump(-1);
    return record;
}
} // namespace

ProjectDocument projectFromGraph(model::GraphSnapshot graph, model::Viewport viewport) {
    ProjectDocument document;
    document.viewport = viewport;
    document.graph = std::move(graph);
    document.root = buildRoot(Json::object(), document.viewport, document.graph);
    return document;
}

std::optional<ProjectDocument> loadProject(const std::filesystem::path& path, std::string& error) {
    std::string text;
    if (!readText(path, text, error)) return std::nullopt;
    auto root = Json::parse(text, error);
    if (!root) return std::nullopt;
    const auto* format = root->find("format");
    const auto* version = root->find("formatVersion");
    if (format == nullptr || format->asString() != "nodsynth.project" || version == nullptr ||
        version->kind() != Json::Kind::number) {
        error = "the file is not a NodSynth project";
        return std::nullopt;
    }
    if (static_cast<int>(version->asNumber()) != kProjectFormatVersion) {
        error = "the project version is not supported";
        return std::nullopt;
    }
    ProjectDocument document;
    document.root = std::move(*root);
    if (const auto* viewport = document.root.find("viewport")) {
        document.viewport.originX = static_cast<float>(viewport->find("originX") ? viewport->find("originX")->asNumber() : 0);
        document.viewport.originY = static_cast<float>(viewport->find("originY") ? viewport->find("originY")->asNumber() : 0);
        document.viewport.zoom = static_cast<float>(viewport->find("zoom") ? viewport->find("zoom")->asNumber() : 1);
    }
    const auto* nodes = document.root.find("nodes");
    const auto* connections = document.root.find("connections");
    if (nodes == nullptr || nodes->kind() != Json::Kind::array || connections == nullptr ||
        connections->kind() != Json::Kind::array) {
        error = "the project is missing its node graph";
        return std::nullopt;
    }
    for (const auto& node : nodes->asArray()) {
        auto record = readNode(node, error);
        if (!record) return std::nullopt;
        document.graph.nodes.push_back(std::move(*record));
    }
    for (const auto& connection : connections->asArray()) {
        const auto* from = connection.find("from");
        const auto* to = connection.find("to");
        if (from == nullptr || to == nullptr || from->find("node") == nullptr || from->find("port") == nullptr ||
            to->find("node") == nullptr || to->find("port") == nullptr) {
            error = "a project connection is incomplete";
            return std::nullopt;
        }
        document.graph.connections.push_back(
            {{model::NodeId{from->find("node")->asString()}, model::PortId{from->find("port")->asString()}},
             {model::NodeId{to->find("node")->asString()}, model::PortId{to->find("port")->asString()}}});
    }
    if(const auto* macros=document.root.find("macros")) {
        if(macros->kind()!=Json::Kind::array || macros->asArray().size()>64) {error="patch-macro: expected at most 64 macros";return std::nullopt;}
        std::set<std::string> ids;
        for(const auto& entry:macros->asArray()) {
            const auto* id=entry.find("id");const auto* mappings=entry.find("mappings");
            if(!id || id->kind()!=Json::Kind::string || id->asString().empty() || !ids.insert(id->asString()).second ||
                !mappings || mappings->kind()!=Json::Kind::array || mappings->asArray().empty() || mappings->asArray().size()>64) {error="patch-macro: invalid identity or mappings";return std::nullopt;}
            model::GraphSnapshot::Macro macro;macro.id=id->asString();
            for(const auto& item:mappings->asArray()) {
                const auto* node=item.find("node");const auto* parameter=item.find("parameter");const auto* low=item.find("minimum");const auto* high=item.find("maximum");const auto* curve=item.find("curve");
                if(!node || node->kind()!=Json::Kind::string || !parameter || parameter->kind()!=Json::Kind::string ||
                    !low || low->kind()!=Json::Kind::number || !high || high->kind()!=Json::Kind::number || !std::isfinite(low->asNumber()) || !std::isfinite(high->asNumber()) ||
                    (curve && curve->kind()!=Json::Kind::string)) {error="patch-macro: invalid target or range";return std::nullopt;}
                macro.mappings.push_back({model::NodeId{node->asString()},model::ParameterId{parameter->asString()},low->asNumber(),high->asNumber(),curve?curve->asString():"linear"});
            }document.graph.macros.push_back(std::move(macro));
        }
    }
    return document;
}

bool saveProject(const std::filesystem::path& path, ProjectDocument& document, std::string& error) {
    if (path.empty()) {
        error = "missing project path";
        return false;
    }
    document.root = buildRoot(document.root, document.viewport, document.graph);
    std::filesystem::path temp = path;
    temp += ".nodsynth-tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to write the temporary project file";
            return false;
        }
        const auto text = document.root.dump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "failed to write the temporary project file";
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }
    if (!replaceFile(temp, path, error)) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}
} // namespace nodsynth::persist
