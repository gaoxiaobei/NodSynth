#include <nodsynth/song/Automation.h>
#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace nodsynth::song {
std::optional<model::GraphSnapshot> migrateProductionGraph(const model::GraphSnapshot& original,const persist::Json& decisions,std::string& error) {
    using persist::Json;
    const auto registry=nodes::builtinRegistry();
    if(!compiler::GraphCompiler{}.compile(original,registry).graph || !validateMacros(original,error)) {
        if(error.empty()) error="migration: source graph is invalid";return std::nullopt;
    }
    if(decisions.kind()!=Json::Kind::object) {error="migration: decisions must be an object";return std::nullopt;}
    for(const auto& [key,value]:decisions.items()) if((key!="cutoff" && key!="gain" && key!="mono") || value.kind()!=Json::Kind::object) {
        error="migration: unknown or invalid decision category "+key;return std::nullopt;
    }
    std::set<std::string> used;
    const auto choice=[&](const std::string& category,const std::string& id)->const Json* {
        const auto* group=decisions.find(category);const auto* entry=group?group->find(id):nullptr;
        if(!entry || entry->kind()!=Json::Kind::object) {error="migration: explicit "+category+" decision required for "+id;return nullptr;}
        used.insert(category+"/"+id);return entry;
    };
    const auto number=[&](const Json& entry,const char* name,double low,double high,double& value) {
        const auto* field=entry.find(name);
        if(!field || field->kind()!=Json::Kind::number || !std::isfinite(field->asNumber()) || field->asNumber()<low || field->asNumber()>high) {
            error=std::string("migration: explicit ")+name+" outside range";return false;
        }value=field->asNumber();return true;
    };
    auto result=original;std::set<model::NodeId> converted;
    for(auto& node:result.nodes) if(const auto* replacement=registry.find(model::NodeTypeId{node.typeId.value+"-v2"})) {
        converted.insert(node.id);node.typeId=replacement->typeId;node.schemaVersion=replacement->schemaVersion;
    }
    std::vector<model::Connection> connections;
    std::size_t nextId=0;
    const auto freshId=[&]() {
        model::NodeId id;
        do {id={"migration-"+std::to_string(nextId++)};} while(std::any_of(result.nodes.begin(),result.nodes.end(),[&](const auto& node){return node.id==id;}));
        return id;
    };
    const auto findNode=[&](const model::NodeId& id)->model::NodeRecord* {
        const auto found=std::find_if(result.nodes.begin(),result.nodes.end(),[&](const auto& node){return node.id==id;});return found==result.nodes.end()?nullptr:&*found;
    };
    for(auto cable:original.connections) {
        auto* target=findNode(cable.to.nodeId);
        if(converted.contains(cable.to.nodeId) && cable.to.portId.value=="cutoff") {
            const auto* entry=choice("cutoff",cable.to.nodeId.value);if(!entry) return std::nullopt;
            const auto* action=entry->find("action");
            if(!action || action->kind()!=Json::Kind::string) {error="migration: cutoff requires action";return std::nullopt;}
            if(action->asString()=="disconnect") {
                double base=0;if(!number(*entry,"baseHz",20,20000,base)) return std::nullopt;
                target->parameters[model::ParameterId{"cutoff"}]=base;continue;
            }
            if(action->asString()!="normalized") {error="migration: cutoff action must be disconnect or normalized";return std::nullopt;}
            double base=0,depth=0;if(!number(*entry,"baseHz",20,20000,base) || !number(*entry,"depthOctaves",-8,8,depth)) return std::nullopt;
            // This is an explicit assertion by the caller; absolute Hz is never inferred as normalized.
            const auto* normalized=entry->find("sourceIsNormalized");
            if(!normalized || normalized->kind()!=Json::Kind::boolean || !normalized->asBool()) {error="migration: normalized source must be explicitly asserted";return std::nullopt;}
            target->parameters[model::ParameterId{"cutoff"}]=base;target->parameters[model::ParameterId{"depth"}]=depth;cable.to.portId={"cutoff-mod"};
        }
        if(converted.contains(cable.to.nodeId) && target->typeId.value=="nod.gain-v2" && cable.to.portId.value=="gain") {
            const auto* entry=choice("gain",cable.to.nodeId.value);if(!entry) return std::nullopt;
            double base=0;if(!number(*entry,"base",0,4,base)) return std::nullopt;target->parameters[model::ParameterId{"gain"}]=base;
        }
        const auto* source=findNode(cable.from.nodeId);
        const auto* sourceSchema=registry.find(source->typeId);const auto* targetSchema=registry.find(target->typeId);
        const auto sourcePort=std::find_if(sourceSchema->ports.begin(),sourceSchema->ports.end(),[&](const auto& port){return port.id==cable.from.portId;});
        const auto targetPort=std::find_if(targetSchema->ports.begin(),targetSchema->ports.end(),[&](const auto& port){return port.id==cable.to.portId;});
        if(sourcePort!=sourceSchema->ports.end() && targetPort!=targetSchema->ports.end() && sourcePort->kind==model::PortKind::audio && sourcePort->channels==1 && targetPort->channels==2) {
            const auto address=cable.from.nodeId.value+"/"+cable.from.portId.value+"->"+cable.to.nodeId.value+"/"+cable.to.portId.value;
            const auto* entry=choice("mono",address);if(!entry) return std::nullopt;
            double pan=0,level=0;if(!number(*entry,"pan",-1,1,pan) || !number(*entry,"level",0,4,level)) return std::nullopt;
            model::NodeRecord bridge;bridge.id=freshId();bridge.typeId={"nod.pan-v2"};bridge.schemaVersion=2;bridge.parameters[model::ParameterId{"pan"}]=pan;
            bridge.position=target->position;const auto panId=bridge.id;result.nodes.push_back(std::move(bridge));
            model::NodeRecord gain;gain.id=freshId();gain.typeId={"nod.gain-v2"};gain.schemaVersion=2;gain.parameters[model::ParameterId{"gain"}]=level;
            const auto gainId=gain.id;result.nodes.push_back(std::move(gain));
            connections.push_back({cable.from,{panId,model::PortId{"audio-in"}}});
            connections.push_back({{panId,model::PortId{"audio-out"}},{gainId,model::PortId{"audio-in"}}});
            cable.from={gainId,model::PortId{"audio-out"}};
        }
        connections.push_back(std::move(cable));
    }
    for(const auto& [category,group]:decisions.items()) for(const auto& [id,entry]:group.items()) if(!used.contains(category+"/"+id)) {
        error="migration: unused decision "+category+"/"+id;return std::nullopt;
    }
    result.connections=std::move(connections);
    if(!compiler::GraphCompiler{}.compile(result,registry).graph || !validateMacros(result,error)) {
        if(error.empty()) error="migration: resulting graph is invalid; stereo-to-mono paths require explicit redesign";return std::nullopt;
    }
    return result;
}
}
