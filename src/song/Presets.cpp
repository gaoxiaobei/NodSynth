#include <nodsynth/song/Presets.h>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <utility>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/render/OfflineRenderer.h>
#include <nodsynth/song/AudioExport.h>
#include <nodsynth/song/Automation.h>
#include <nodsynth/persist/ProjectFile.h>

namespace nodsynth::song {
namespace {
struct CatalogEntry {
    const char* id;
    int version;
    const char* role;
    const char* name;
    const char* const* tags;
    std::size_t tagCount;
    const char* file;
};

const char* kKickTags[] = {"drums", "kick", "percussion"};
const char* kClapTags[] = {"drums", "clap", "snare", "percussion"};
const char* kClosedHatTags[] = {"drums", "hat", "closed", "percussion"};
const char* kOpenHatTags[] = {"drums", "hat", "open", "percussion"};
const char* kBassTags[] = {"bass", "synth"};
const char* kLeadTags[] = {"lead", "synth", "melody"};
const char* kPadTags[] = {"pad", "synth", "harmony"};
const char* kTranceTags[] = {"trance", "synth"};
const char* kHouseTags[] = {"house", "synth"};

const CatalogEntry kCatalog[] = {
    {"kick", 1, "kick", "Analog Kick", kKickTags, 3, "kick.json"},
    {"snare-clap", 1, "clap", "Snare Clap", kClapTags, 4, "snare-clap.json"},
    {"closed-hat", 1, "hat", "Closed Hat", kClosedHatTags, 4, "closed-hat.json"},
    {"open-hat", 1, "hat", "Open Hat", kOpenHatTags, 4, "open-hat.json"},
    {"bass", 1, "bass", "Saw Bass", kBassTags, 2, "bass.json"},
    {"lead", 1, "lead", "Square Lead", kLeadTags, 3, "lead.json"},
    {"pad", 1, "pad", "Soft Pad", kPadTags, 3, "pad.json"},
    {"trance-lead", 1, "lead", "Trance Layered Detuned Saw", kTranceTags, 2, "trance-lead.json"},
    {"trance-bass", 1, "bass", "Trance Offbeat Saw Bass", kTranceTags, 2, "trance-bass.json"},
    {"trance-pad", 1, "pad", "Trance Saw Pad", kTranceTags, 2, "trance-pad.json"},
    {"house-bass", 1, "bass", "House Square Bass", kHouseTags, 2, "house-bass.json"},
    {"house-chord", 1, "pad", "House Chord Stab", kHouseTags, 2, "house-chord.json"},
    {"stereo-unison-lead", 2, "lead", "Stereo Unison Lead", kLeadTags, 3, "stereo-unison-lead.json"},
    {"stereo-unison-pad", 2, "pad", "Stereo Unison Pad", kPadTags, 3, "stereo-unison-pad.json"},
    {"production-lead",1,"lead","Production Stereo Lead",kLeadTags,3,"production-roles/production-lead.json"},
    {"production-pad",1,"pad","Production Stereo Pad",kPadTags,3,"production-roles/production-pad.json"},
    {"production-pluck",1,"pluck","Production Pluck",kLeadTags,3,"production-roles/production-pluck.json"},
    {"production-offbeat-bass",1,"bass","Production Offbeat Bass",kBassTags,2,"production-roles/production-offbeat-bass.json"},
    {"production-sub",1,"sub","Production Mono Sub",kBassTags,2,"production-roles/production-sub.json"},
    {"production-riser",1,"riser","Production Riser",kTranceTags,2,"production-roles/production-riser.json"},
    {"production-impact",1,"impact","Production Impact",kTranceTags,2,"production-roles/production-impact.json"},
};

PresetInfo fromEntry(const CatalogEntry& entry, const std::filesystem::path& presetsRoot) {
    PresetInfo info;
    info.id = entry.id;
    info.version = entry.version;
    info.role = entry.role;
    info.name = entry.name;
    info.patchPath = entry.file;
    info.tags.assign(entry.tags, entry.tags + entry.tagCount);
    info.hash = hashFile(presetsRoot / entry.file);
    std::string error;
    if (auto graph = render::loadPatch(presetsRoot / entry.file, error)) info.parameters = parametersJson(*graph);
    if(auto project=persist::loadProject(presetsRoot/entry.file,error))
        if(const auto* metadata=project->root.find("production")) info.production=*metadata;
    return info;
}

const CatalogEntry* catalogEntry(std::string_view id) {
    for (const auto& entry : kCatalog) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

std::string storedPatchPath(const PresetInfo& info) {
    return (std::filesystem::path("presets") / info.patchPath).generic_string();
}

midi::File auditionMidi(const PresetAuditionOptions& options) {
    midi::File file;
    file.format = 0;
    file.ppq = 480;
    file.trackCount = 1;
    const auto ticks = static_cast<std::uint32_t>(std::max(1.0, options.noteSeconds) * file.ppq * 2.0);
    midi::Event on;
    on.tick = 0;
    on.track = 0;
    on.sequence = 0;
    on.channel = 0;
    on.hasChannel = true;
    on.kind = midi::EventKind::noteOn;
    on.data1 = options.pitch;
    on.data2 = options.velocity;
    midi::Event off = on;
    off.tick = ticks;
    off.sequence = 1;
    off.kind = midi::EventKind::noteOff;
    off.data2 = 0;
    file.events = {on, off};
    file.endTick = ticks + file.ppq;
    file.tempo = {{0, midi::kDefaultTempoUs}};
    file.timeSignatures = {{0, 4, 4}};
    return file;
}
} // namespace

std::filesystem::path defaultPresetsRoot() {
    if (const char* env = std::getenv("NODSYNTH_PRESETS"); env != nullptr && env[0] != '\0') return env;
    return std::filesystem::path(NOD_BUILTIN_PRESETS);
}

std::vector<PresetInfo> listPresets(const std::filesystem::path& presetsRoot, std::optional<std::string> role) {
    std::vector<PresetInfo> listed;
    for (const auto& entry : kCatalog) {
        if (role && *role != entry.role) continue;
        listed.push_back(fromEntry(entry, presetsRoot));
    }
    return listed;
}

std::optional<PresetInfo> findPreset(const std::filesystem::path& presetsRoot, std::string_view id) {
    const auto* entry = catalogEntry(id);
    if (entry == nullptr) return std::nullopt;
    return fromEntry(*entry, presetsRoot);
}

persist::Json presetJson(const PresetInfo& info) {
    persist::Json json = persist::Json::object();
    json.set("id", persist::Json::string(info.id));
    json.set("version", persist::Json::number(info.version));
    json.set("role", persist::Json::string(info.role));
    json.set("name", persist::Json::string(info.name));
    persist::Json tags = persist::Json::array();
    for (const auto& tag : info.tags) tags.push(persist::Json::string(tag));
    json.set("tags", std::move(tags));
    json.set("patchPath", persist::Json::string(storedPatchPath(info)));
    json.set("hash", persist::Json::string(info.hash));
    json.set("parameters", info.parameters);
    json.set("production",info.production);
    json.set("dependencies", persist::Json::array());
    json.set("auditionStatus", persist::Json::string("unheard"));
    json.set("levelPolicy", persist::Json::string("patch levels reserve headroom; use representative audition to measure actual peak"));
    return json;
}

persist::Json listJson(const std::vector<PresetInfo>& presets) {
    persist::Json json = persist::Json::object();
    persist::Json items = persist::Json::array();
    for (const auto& preset : presets) items.push(presetJson(preset));
    json.set("presets", std::move(items));
    json.set("count", persist::Json::number(static_cast<double>(presets.size())));
    return json;
}

std::vector<PresetInfo> searchPresets(const std::filesystem::path& root,std::string_view text,std::optional<std::string> role) {
    auto lower=[](std::string value) {
        for (auto& c:value) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    std::vector<std::string> terms;std::istringstream words(lower(std::string(text)));std::string term;
    while (words>>term) terms.push_back(term);
    auto presets=listPresets(root,std::move(role));
    std::erase_if(presets,[&](const auto& preset) {
        std::string content=preset.id+" "+preset.role+" "+preset.name;
        for (const auto& tag:preset.tags) content+=" "+tag;
        content=lower(std::move(content));
        return std::any_of(terms.begin(),terms.end(),[&](const auto& value){return content.find(value)==std::string::npos;});
    });
    return presets;
}

bool bindPreset(
    SongDocument& song,
    const std::string& trackId,
    std::string_view presetId,
    const std::filesystem::path& presetsRoot,
    std::string& error) {
    const auto found = findPreset(presetsRoot, presetId);
    if (!found) {
        error = "preset was not found";
        return false;
    }
    const auto file = presetsRoot / found->patchPath;
    if (!std::filesystem::exists(file) || found->hash.empty()) {
        error = "preset patch file is missing";
        return false;
    }
    const auto absolute = std::filesystem::absolute(file).u8string();
    if (!bindPatch(song, trackId, {reinterpret_cast<const char*>(absolute.data()), absolute.size()}, file, error)) return false;
    for (auto& resource : song.resources) {
        if (resource.path == std::string(reinterpret_cast<const char*>(absolute.data()), absolute.size())) {
            resource.presetId = found->id;
            resource.presetVersion = found->version;
        }
    }
    return true;
}

render::RenderReport renderPresetAudition(
    std::string_view presetId, const std::filesystem::path& outputWav, const PresetAuditionOptions& options) {
    render::RenderReport report;
    const auto root = options.presetsRoot.empty() ? defaultPresetsRoot() : options.presetsRoot;
    const auto found = findPreset(root, presetId);
    if (!found) {
        report.code = "unknown-preset";
        report.message = "preset was not found";
        return report;
    }
    std::string error;
    auto graph = render::loadPatch(root / found->patchPath, error);
    if (!graph) {
        report.code = "missing-patch";
        report.message = error.empty() ? "failed to load the preset patch" : error;
        return report;
    }
    render::RenderOptions renderOptions;
    renderOptions.sampleRate = options.sampleRate;
    renderOptions.blockSize = options.blockSize;
    renderOptions.tailSeconds = options.tailSeconds;
    renderOptions.tailMode = render::TailMode::fixed;
    renderOptions.patchHash = found->hash;
    auto rendered = render::renderMidi(*graph, auditionMidi(options), renderOptions, outputWav);
    if (rendered.ok) {
        runtime::WavData audio;
        double gain = 1;
        if (!runtime::readWav(outputWav, audio, error) || !writePcm16(outputWav, audio, false, gain, error)) {
            rendered.ok = false; rendered.code = "export-failed"; rendered.message = error;
        }
    }
    return rendered;
}
} // namespace nodsynth::song
