#include <nodsynth/song/Workflow.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <nodsynth/song/Presets.h>

namespace nodsynth::song {
namespace {
using persist::Json;
bool integer(const Json* value, std::uint32_t& out) {
    if (!value || value->kind()!=Json::Kind::number) return false;
    const auto n=value->asNumber();
    if (!std::isfinite(n) || n<0 || n>UINT32_MAX || std::floor(n)!=n) return false;
    out=static_cast<std::uint32_t>(n);return true;
}
Json effect(const char* id, const char* type, std::initializer_list<std::pair<const char*,double>> values) {
    auto item=Json::object(), parameters=Json::object();
    item.set("id",Json::string(id));item.set("type",Json::string(type));
    for (const auto& [key,value]:values) parameters.set(key,Json::number(value));
    item.set("parameters",std::move(parameters));return item;
}
}

std::string trackRole(const SongDocument& song, const Track& track) {
    if (!track.role.empty()) return track.role;
    for (const auto& instrument:song.instruments) if (instrument.id==track.instrumentId)
        for (const auto& resource:song.resources) if (resource.id==instrument.resourceId && !resource.presetId.empty())
            if (const auto preset=findPreset(defaultPresetsRoot(),resource.presetId)) return preset->role;
    return {};
}

Json startingChain(const std::string& role) {
    auto chain=Json::array();
    if (role=="lead" || role=="pluck" || role=="pad") {
        chain.push(effect("starter-lowcut","eq",{{"mode",1},{"frequency",role=="pad"?160.:120.},{"q",.707}}));
    } else if (role=="kick" || role=="bass" || role=="sub") {
        chain.push(effect("starter-lowcut","eq",{{"mode",1},{"frequency",25},{"q",.707}}));
    } else if (role=="hat" || role=="clap" || role=="snare" || role=="percussion") {
        chain.push(effect("starter-lowcut","eq",{{"mode",1},{"frequency",role=="hat"?400.:120.},{"q",.707}}));
    }
    return chain;
}

Json workflowJson(const SongDocument& song,const QueryOptions& options) {
    auto result=Json::object(), tracks=Json::array(), sections=Json::array();
    result.set("revision",Json::number(static_cast<double>(song.revision)));
    for (const auto& track:song.tracks) {
        if (options.trackId && track.id!=*options.trackId) continue;
        auto item=Json::object();const auto role=trackRole(song,track);
        if (options.role && role!=*options.role) continue;
        item.set("track",Json::string(track.id));item.set("name",Json::string(track.name));
        item.set("role",role.empty()?Json::null():Json::string(role));
        item.set("roleSource",Json::string(!track.role.empty()?"explicit":role.empty()?"unassigned":"preset"));
        item.set("selectionRequired",Json::boolean(role.empty()));
        auto batch=Json::object(),commands=Json::array(),command=Json::object();
        command.set("op",Json::string("set-inserts"));command.set("target",Json::string(track.id));
        command.set("inserts",startingChain(role));commands.push(std::move(command));batch.set("commands",std::move(commands));
        batch.set("baseRevision",Json::number(static_cast<double>(song.revision)));
        batch.set("schemaVersion",Json::number(1));
        item.set("startingChainProposal",startingChain(role).asArray().empty()?Json::null():std::move(batch));
        item.set("chainPolicy",Json::string("review before applying; replaces inserts; conservative highpass only, tune by listening"));
        tracks.push(std::move(item));
    }
    for (const auto& section:song.sections) {
        auto item=Json::object();item.set("id",Json::string(section.id));item.set("name",Json::string(section.name));
        item.set("startTick",Json::number(section.startTick));item.set("endTick",Json::number(section.endTick));
        sections.push(std::move(item));
    }
    result.set("tracks",std::move(tracks));result.set("sections",std::move(sections));
    result.set("capabilityQuery",Json::string("nod song query FILE --view capabilities --json"));
    result.set("structurePolicy",Json::string("sections are named ranges; repeat-phrase copies clipped notes only, leaving automation, tempo and performance events explicit"));
    return result;
}

bool applyWorkflow(SongDocument& song,const Json& command,Json& idMap,std::string& error) {
    const auto op=command.find("op")->asString();
    if (op=="set-role") {
        const auto* id=command.find("track");const auto* role=command.find("role");
        if (!id || !role || role->kind()!=Json::Kind::string) {error="set-role requires track and string role";return false;}
        for (auto& track:song.tracks) if (track.id==id->asString()) {track.role=role->asString();return true;}
        error="track was not found";return false;
    }
    const auto* id=command.find("id");
    if (op=="set-section" || op=="delete-section") {
        if (!id || id->kind()!=Json::Kind::string || id->asString().empty()) {error="section requires a nonempty id";return false;}
        auto found=std::find_if(song.sections.begin(),song.sections.end(),[&](const auto& s){return s.id==id->asString();});
        if (op=="delete-section") {
            if (found==song.sections.end()) {error="section was not found";return false;}
            song.sections.erase(found);return true;
        }
        Section section;section.id=id->asString();section.name=section.id;
        if (const auto* name=command.find("name")) {
            if (name->kind()!=Json::Kind::string) {error="section name must be a string";return false;}
            section.name=name->asString();
        }
        if (!integer(command.find("startTick"),section.startTick) || !integer(command.find("endTick"),section.endTick) || section.startTick>=section.endTick) {
            error="section requires increasing startTick/endTick";return false;
        }
        if (found==song.sections.end()) song.sections.push_back(section);else *found=section;
        return true;
    }
    std::uint32_t start=0,end=0,destination=0;
    if (const auto* source=command.find("section")) {
        const auto found=std::find_if(song.sections.begin(),song.sections.end(),[&](const auto& s){return s.id==source->asString();});
        if (found==song.sections.end()) {error="source section was not found";return false;}
        start=found->startTick;end=found->endTick;
    } else if (!integer(command.find("startTick"),start) || !integer(command.find("endTick"),end)) {
        error="repeat-phrase requires section or startTick/endTick";return false;
    }
    if (start>=end || !integer(command.find("destinationTick"),destination) ||
        static_cast<std::uint64_t>(destination)+end-start>UINT32_MAX || !id || id->kind()!=Json::Kind::string || id->asString().empty()) {
        error="repeat-phrase requires a unique id, increasing range and valid destinationTick";return false;
    }
    const auto* selected=command.find("track");
    const auto destinationEnd=destination+end-start;
    if (song.songRangeEndTick && destinationEnd>*song.songRangeEndTick &&
        !(command.find("extendSongRange") && command.find("extendSongRange")->asBool())) {
        error="phrase exceeds song range; set extendSongRange explicitly";return false;
    }
    bool selectedFound=!selected;std::set<std::string> ids;
    for (const auto& track:song.tracks) for (const auto& clip:track.clips) {
        ids.insert(clip.id);for (const auto& note:clip.notes) ids.insert(note.id);
    }
    std::vector<std::pair<std::size_t,Clip>> copies;
    for (std::size_t i=0;i<song.tracks.size();++i) {
        const auto& track=song.tracks[i];if (selected && track.id!=selected->asString()) continue;
        selectedFound=true;Clip copy;copy.id=id->asString()+":"+track.id;copy.startTick=destination;copy.length=end-start;
        for (const auto& clip:track.clips) for (const auto& note:clip.notes) {
            const auto onset=static_cast<std::uint64_t>(clip.startTick)+note.tick;
            const auto finish=onset+note.duration;
            if (onset>=end || finish<=start) continue;
            auto n=note;n.tick=static_cast<std::uint32_t>(std::max<std::uint64_t>(start,onset)-start);
            n.duration=static_cast<std::uint32_t>(std::min<std::uint64_t>(end,finish)-std::max<std::uint64_t>(start,onset));
            n.id=copy.id+":note:"+std::to_string(copy.notes.size()+1);
            if (!ids.insert(n.id).second) {error="repeat-phrase note id collision";return false;}
            copy.notes.push_back(std::move(n));
        }
        if (copy.notes.empty()) continue;
        if (!ids.insert(copy.id).second) {error="repeat-phrase clip id collision";return false;}
        copies.emplace_back(i,std::move(copy));
    }
    if (!selectedFound || copies.empty()) {error="repeat-phrase selection has no notes";return false;}
    for (auto& [i,copy]:copies) {idMap.set(copy.id,Json::string(copy.id));song.tracks[i].clips.push_back(std::move(copy));}
    if (song.songRangeEndTick && destinationEnd>*song.songRangeEndTick) song.songRangeEndTick=destinationEnd;
    return true;
}

Json workflowMetrics(const Json& trace) {
    auto report=Json::object();const auto reject=[&](const char* message) {
        report.set("status",Json::string("rejected"));report.set("message",Json::string(message));return report;
    };
    const auto* actor=trace.find("actor");const auto* snapshot=trace.find("resourceSnapshotHash");const auto* events=trace.find("events");
    if (!actor || actor->kind()!=Json::Kind::string || actor->asString().empty() || !snapshot || snapshot->kind()!=Json::Kind::string || snapshot->asString().empty() ||
        !events || events->kind()!=Json::Kind::array) return reject("trace requires actor, resourceSnapshotHash and events array");
    std::uint64_t commands=0,batches=0,retries=0,lookups=0,interventions=0,invalidLanes=0,toolCalls=0;
    std::set<std::string> intents;
    for (const auto& event:events->asArray()) {
        const auto* kind=event.find("kind");if (!kind || kind->kind()!=Json::Kind::string) return reject("event requires kind");
        if (kind->asString()=="tool-call") {
            const auto* tool=event.find("tool");const auto* arguments=event.find("arguments");const auto* exitCode=event.find("exitCode");
            if (!tool || tool->kind()!=Json::Kind::string || tool->asString().empty() || !arguments || arguments->kind()!=Json::Kind::array ||
                !exitCode || exitCode->kind()!=Json::Kind::number || !std::isfinite(exitCode->asNumber()) || std::floor(exitCode->asNumber())!=exitCode->asNumber())
                return reject("tool-call requires tool, arguments array and integer exitCode");
            ++toolCalls;
        } else if (kind->asString()=="source-lookup") ++lookups;
        else if (kind->asString()=="human-intervention") ++interventions;
        else if (kind->asString()=="command-batch") {
            const auto* batch=event.find("commands");const auto* intent=event.find("intentId");const auto* outcome=event.find("outcome");
            if (!batch || batch->kind()!=Json::Kind::array || !intent || intent->kind()!=Json::Kind::string || intent->asString().empty() ||
                !outcome || (outcome->asString()!="ok" && outcome->asString()!="rejected")) return reject("command-batch requires commands, intentId and ok/rejected outcome");
            for (const auto& command:batch->asArray()) if (!command.find("op") || command.find("op")->kind()!=Json::Kind::string) return reject("logged command requires op");
            commands+=batch->asArray().size();++batches;if (!intents.insert(intent->asString()).second) ++retries;
            if (outcome->asString()=="rejected") {
                const auto* code=event.find("code");
                if (!code || code->kind()!=Json::Kind::string) return reject("rejected command-batch requires diagnostic code");
                if (code->asString()=="control-override" || code->asString()=="unsupported-target" || code->asString()=="unsupported-backend")
                    for (const auto& command:batch->asArray()) if (command.find("op")->asString()=="set-parameter-automation" || command.find("op")->asString()=="set-effect-automation") ++invalidLanes;
            }
        } else return reject("unknown workflow event kind");
    }
    report.set("status",Json::string("ok"));report.set("actor",*actor);report.set("resourceSnapshotHash",*snapshot);
    report.set("commands",Json::number(static_cast<double>(commands)));report.set("batches",Json::number(static_cast<double>(batches)));
    report.set("toolCalls",Json::number(static_cast<double>(toolCalls)));
    report.set("retries",Json::number(static_cast<double>(retries)));report.set("sourceLookups",Json::number(static_cast<double>(lookups)));
    report.set("humanInterventions",Json::number(static_cast<double>(interventions)));report.set("invalidLaneAttempts",Json::number(static_cast<double>(invalidLanes)));
    report.set("evidenceScope",Json::string("counts declared trace events; rejected lane commands are attempts, not proof each individual lane failed; compare actors only with the same frozen resources and task"));
    return report;
}
} // namespace nodsynth::song
