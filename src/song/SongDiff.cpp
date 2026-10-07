#include <nodsynth/song/SongDocument.h>

#include <algorithm>
#include <map>
#include <set>

namespace nodsynth::song {
namespace {
using persist::Json;
struct Entity { std::string type, id, track; Json data; };
using Entities = std::map<std::string, Entity>;

Json without(const Json& source, const std::set<std::string>& keys) {
    auto out = Json::object();
    for (const auto& [key, value] : source.items()) if (!keys.contains(key)) out.set(key, value);
    return out;
}

Entities flatten(const SongDocument& song) {
    Entities entities;
    const auto document = toJson(song);
    auto root = Json::object();
    for (const auto* key : {"ppq", "songRangeEndTick", "tempo", "timeSignatures", "masterInserts", "sections"})
        if (const auto* value = document.find(key)) root.set(key, *value);
    entities.emplace("song:", Entity{"song", "", "", root});
    const auto add = [&](const char* type, const Json& item, std::string track, Json data) {
        const auto id = item.find("id")->asString();
        entities.emplace(std::string(type) + ':' + id, Entity{type, id, std::move(track), std::move(data)});
    };
    for (const auto* type : {"resource", "instrument", "bus"}) {
        const auto* list = document.find(std::string(type)=="bus" ? "buses" : std::string(type) + "s");
        for (const auto& item : list->asArray()) add(type, item, "", item);
    }
    for (const auto& track : document.find("tracks")->asArray()) {
        const auto trackId = track.find("id")->asString();
        add("track", track, trackId, without(track, {"clips"}));
        for (const auto& clip : track.find("clips")->asArray()) {
            auto clipData = without(clip, {"notes"});
            clipData.set("track", Json::string(trackId));
            add("clip", clip, trackId, std::move(clipData));
            for (const auto& note : clip.find("notes")->asArray()) {
                auto noteData = note;
                noteData.set("track", Json::string(trackId));
                noteData.set("clip", *clip.find("id"));
                noteData.set("absoluteTick", Json::number(clip.find("startTick")->asNumber() + note.find("tick")->asNumber()));
                add("note", note, trackId, std::move(noteData));
            }
        }
    }
    return entities;
}

Json compact(const Json& value, bool expand) {
    if (expand || value.kind() != Json::Kind::array) return value;
    const auto text = value.dump(-1);
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) { hash ^= byte; hash *= 1099511628211ull; }
    auto result = Json::object();
    result.set("count", Json::number(static_cast<double>(value.asArray().size())));
    result.set("hash", Json::string(std::to_string(hash)));
    return result;
}
} // namespace

persist::Json semanticDiff(const SongDocument& before, const SongDocument& after, bool expandAutomation, bool expandNotes) {
    const auto oldEntities = flatten(before), newEntities = flatten(after);
    std::set<std::string> keys, tracks;
    for (const auto& [key, entity] : oldEntities) keys.insert(key);
    for (const auto& [key, entity] : newEntities) keys.insert(key);
    auto added = Json::array(), removed = Json::array(), changes = Json::array();
    std::map<std::string, std::map<std::string, std::size_t>> counts;
    std::map<std::string, std::map<std::string, std::size_t>> notes;
    std::size_t entitiesChanged = 0, recordCount = 0;
    for (const auto& key : keys) {
        const auto oldIt = oldEntities.find(key), newIt = newEntities.find(key);
        const bool haveOld = oldIt != oldEntities.end(), haveNew = newIt != newEntities.end();
        const auto& entity = haveNew ? newIt->second : oldIt->second;
        if (haveOld && haveNew && oldIt->second.data.dump(-1) == newIt->second.data.dump(-1)) continue;
        ++entitiesChanged;
        const std::string category = !haveOld ? "added" : !haveNew ? "removed" : "modified";
        ++counts[entity.type][category];
        if (!entity.track.empty()) tracks.insert(entity.track);
        if (entity.type == "note") ++notes[entity.track][category];
        if (!haveOld || !haveNew) {
            ++recordCount;
            if (entity.type == "note" && !expandNotes) continue;
            auto record = Json::object();
            record.set("entityType", Json::string(entity.type)); record.set("id", Json::string(entity.id));
            auto data = entity.data;
            if (!expandAutomation && entity.type == "track")
                for (const auto* field : {"gainAutomation", "parameterAutomation", "performance"})
                    if (const auto* value = data.find(field)) { auto summary = compact(*value, false); data.set(field, std::move(summary)); }
            record.set("data", std::move(data));
            (haveNew ? added : removed).push(std::move(record));
        } else {
            std::set<std::string> fields;
            for (const auto& [field, value] : oldIt->second.data.items()) fields.insert(field);
            for (const auto& [field, value] : newIt->second.data.items()) fields.insert(field);
            for (const auto& field : fields) {
                const auto* oldValue = oldIt->second.data.find(field); const auto* newValue = newIt->second.data.find(field);
                const auto a = oldValue ? *oldValue : Json::null(), b = newValue ? *newValue : Json::null();
                if (a.dump(-1) == b.dump(-1)) continue;
                ++recordCount;
                if (entity.type == "note" && !expandNotes) continue;
                auto record = Json::object();
                record.set("entityType", Json::string(entity.type)); record.set("id", Json::string(entity.id));
                record.set("field", Json::string(field));
                record.set("before", compact(a, expandAutomation)); record.set("after", compact(b, expandAutomation));
                changes.push(std::move(record));
            }
        }
    }
    auto byType = Json::object();
    for (const auto& [type, tally] : counts) {
        auto item = Json::object();
        for (const auto* category : {"added", "removed", "modified"})
            item.set(category, Json::number(tally.contains(category) ? static_cast<double>(tally.at(category)) : 0));
        byType.set(type, std::move(item));
    }
    auto summaries = Json::array();
    for (const auto& trackId : tracks) {
        auto item = Json::object(); item.set("track", Json::string(trackId));
        for (const auto* category : {"added", "removed", "modified"}) item.set(category, Json::number(static_cast<double>(notes[trackId][category])));
        std::size_t total = 0; std::uint64_t start = UINT64_MAX, end = 0;
        for (const auto& [key, entity] : newEntities) if (entity.type == "note" && entity.track == trackId) {
            ++total;
            const auto tick = static_cast<std::uint64_t>(entity.data.find("absoluteTick")->asNumber());
            start = std::min(start, tick); end = std::max(end, tick + static_cast<std::uint64_t>(entity.data.find("duration")->asNumber()));
        }
        item.set("total", Json::number(static_cast<double>(total)));
        item.set("startTick", total ? Json::number(static_cast<double>(start)) : Json::null());
        item.set("endTick", total ? Json::number(static_cast<double>(end)) : Json::null()); summaries.push(std::move(item));
    }
    auto affected = Json::array(); for (const auto& id : tracks) affected.push(Json::string(id));
    auto range = Json::object(); range.set("startTick", Json::number(0));
    range.set("endTick", Json::number(std::max(endTick(before), endTick(after))));
    auto diff = Json::object(); diff.set("schemaVersion", Json::number(2));
    diff.set("changedEntityCount", Json::number(static_cast<double>(entitiesChanged)));
    diff.set("changeRecordCount", Json::number(static_cast<double>(recordCount)));
    diff.set("legacyChangedEntityCount", Json::number(static_cast<double>(recordCount)));
    diff.set("entities", std::move(byType)); diff.set("notesSummary", std::move(summaries));
    diff.set("tracks", std::move(affected)); diff.set("invalidateRange", std::move(range));
    diff.set("added", std::move(added)); diff.set("removed", std::move(removed)); diff.set("changes", std::move(changes));
    return diff;
}
} // namespace nodsynth::song
