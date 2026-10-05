#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <nodsynth/midi/Smf.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/ModelClient.h>
#include <nodsynth/song/Presets.h>
#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/SongRenderer.h>
#include <nodsynth/song/AudioExport.h>

namespace {
std::filesystem::path filePath(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void usage() {
    std::cerr << "usage: nod song create FILE [--bpm N] [--meter N/N] [--bars N] [--ppq N] [--json]\n"
              << "       nod song import-midi FILE --output FILE [--map TRACK:CHANNEL=PATCH]...\n"
              << "       nod song validate FILE [--json]\n"
              << "       nod song apply FILE --commands FILE [--expect-revision N] [--output FILE] [--dry-run] [--json]\n"
              << "                 [--diff-detail summary|notes|full]\n"
              << "       nod song undo FILE [--output FILE] [--diff-detail summary|notes|full] [--json]\n"
              << "       nod song redo FILE [--output FILE] [--diff-detail summary|notes|full] [--json]\n"
              << "       nod song query FILE [--view summary|tracks|notes|capabilities|legacy]\n"
              << "                 [--track ID] [--bars N:N] [--start-tick N] [--end-tick N]\n"
              << "                 [--limit N] [--cursor TEXT] [--onset-only] [--json]\n"
              << "       nod song propose FILE --adapter EXE --instruction TEXT --output FILE [--adapter-arg ARG]... [--json]\n"
              << "       nod song export-midi FILE --output FILE\n"
              << "       nod preset list [--role ROLE] [--json]\n"
              << "       nod preset inspect ID [--json]\n"
              << "       nod preset audition ID --output FILE [--json]\n"
              << "       nod analyze FILE [--json]\n"
              << "       nod compare A.wav B.wav [--match-loudness] [--json]\n"
              << "       nod play AUDIO --report FILE [--records FILE] [--json]\n"
              << "       nod audition record AUDIO --report FILE --heard --reviewer NAME [--records FILE] [--json]\n"
              << "       nod audition query AUDIO [--records FILE] [--json]\n"
              << "       nod render FILE --output FILE [--stems DIR] [--report FILE]\n"
              << "                 [--sample-rate 48000] [--block-size 128] [--tail-seconds 2]\n"
              << "                 [--start-tick N] [--end-tick N] [--bars N:N] [--tail-threshold AMPLITUDE] [--max-tail-seconds 8]\n"
              << "                 [--cache-dir DIR] [--no-cache] [--quality final|draft] [--freeze-external]\n"
              << "                 [--format float32|pcm16] [--preview-output FILE] [--pcm-overflow reject|attenuate]\n"
              << "                 [--loose] [--json] [--fluidsynth EXE] [--vst3-worker EXE]\n"
              << "Song track IDs are stable. --map TRACK:CHANNEL selects the MIDI source stream, not the Song track id.\n"
              << "--bars 9:13 is 1-based and half-open: bar 9 up to the start of bar 13.\n"
              << "Playback only records playbackStarted. Human confirmation records heard, not approval.\n";
}

int fail(int code, const std::string& message, bool json) {
    if (json) {
        nodsynth::persist::Json report = nodsynth::persist::Json::object();
        report.set("status", nodsynth::persist::Json::string("rejected"));
        report.set("message", nodsynth::persist::Json::string(message));
        std::cout << report.dump() << '\n';
    } else {
        std::cerr << message << '\n';
    }
    return code;
}

int classify(const std::string& code) {
    if (code == "revision-conflict" || code == "request-conflict") return 5;
    if (code == "output-io") return 4;
    if (code == "missing-patch" || code == "missing-instrument" || code == "missing-resource" || code == "resource-hash" ||
        code == "compile-failed" || code == "prepare-failed" || code == "missing-adapter") {
        return 2;
    }
    if (code == "bad-command" || code == "bad-midi" || code == "invalid-song" || code == "nothing-to-undo" || code == "nothing-to-redo" ||
        code == "bad-history") {
        return 1;
    }
    return 3;
}

const char* argument(int argc, char** argv, std::string_view name, int& index) {
    if (std::string_view(argv[index]) != name || index + 1 >= argc) return nullptr;
    ++index;
    return argv[index];
}

bool parseMap(std::string_view text, nodsynth::song::StreamMap& map) {
    const auto colon = text.find(':');
    const auto equals = text.find('=');
    if (colon == std::string_view::npos || equals == std::string_view::npos || colon == 0 || equals < colon + 2) return false;
    try {
        const auto track = std::stoul(std::string(text.substr(0, colon)));
        const auto channel = std::stoul(std::string(text.substr(colon + 1, equals - colon - 1)));
        if (track > 65535 || channel > 15) return false;
        map.sourceTrack = static_cast<std::uint16_t>(track);
        map.sourceChannel = static_cast<std::uint8_t>(channel);
    } catch (...) {
        return false;
    }
    map.patchPath = std::string(text.substr(equals + 1));
    return !map.patchPath.empty();
}
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    int wideCount = 0;
    auto** wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    std::vector<std::string> utf8Arguments;
    std::vector<char*> pointers;
    if (wide) {
        for (int i = 0; i < wideCount; ++i) {
            const auto size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
            text.pop_back(); utf8Arguments.push_back(std::move(text));
        }
        LocalFree(wide);
        for (auto& text : utf8Arguments) pointers.push_back(text.data());
        argc = wideCount; argv = pointers.data();
    }
#endif
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string_view command = argv[1];
    try {
        if (command == "song") {
            if (argc < 3) {
                usage();
                return 1;
            }
            const std::string_view sub = argv[2];
            std::filesystem::path input;
            std::filesystem::path output;
            std::filesystem::path commands;
            std::filesystem::path adapterExe;
            std::string instruction;
            std::vector<std::string> adapterArgs;
            std::optional<std::uint64_t> expectRevision;
            std::optional<std::uint32_t> startTick;
            std::optional<std::uint32_t> endTick;
            std::optional<std::uint32_t> limit;
            std::string view;
            std::string trackId;
            std::string bars;
            std::string cursor;
            std::string meter;
            double bpm = 120;
            std::uint16_t ppq = 480;
            bool json = false;
            bool sawInput = false;
            bool dryRun = false;
            bool onsetOnly = false;
            bool expandAutomation = false;
            bool renderReady = false;
            std::string diffDetail;
            nodsynth::song::ImportOptions options;
            for (int index = 3; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") {
                    json = true;
                } else if (token == "--dry-run") {
                    dryRun = true;
                } else if (token == "--onset-only") {
                    onsetOnly = true;
                } else if (token == "--expand-automation") {
                    expandAutomation = true;
                } else if (token == "--render-ready") {
                    renderReady = true;
                } else if (const char* value = argument(argc, argv, "--diff-detail", index)) {
                    diffDetail = value;
                } else if (const char* value = argument(argc, argv, "--output", index)) {
                    output = filePath(value);
                } else if (const char* value = argument(argc, argv, "--commands", index)) {
                    commands = filePath(value);
                } else if (const char* value = argument(argc, argv, "--adapter", index)) {
                    adapterExe = filePath(value);
                } else if (const char* value = argument(argc, argv, "--instruction", index)) {
                    instruction = value;
                } else if (const char* value = argument(argc, argv, "--adapter-arg", index)) {
                    adapterArgs.emplace_back(value);
                } else if (const char* value = argument(argc, argv, "--expect-revision", index)) {
                    expectRevision = static_cast<std::uint64_t>(std::stoull(value));
                } else if (const char* value = argument(argc, argv, "--start-tick", index)) {
                    startTick = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--end-tick", index)) {
                    endTick = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--view", index)) {
                    view = value;
                } else if (const char* value = argument(argc, argv, "--track", index)) {
                    trackId = value;
                } else if (const char* value = argument(argc, argv, "--bars", index)) {
                    bars = value;
                } else if (const char* value = argument(argc, argv, "--limit", index)) {
                    limit = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--cursor", index)) {
                    cursor = value;
                } else if (const char* value = argument(argc, argv, "--bpm", index)) {
                    bpm = std::stod(value);
                } else if (const char* value = argument(argc, argv, "--meter", index)) {
                    meter = value;
                } else if (const char* value = argument(argc, argv, "--ppq", index)) {
                    ppq = static_cast<std::uint16_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--map", index)) {
                    nodsynth::song::StreamMap map;
                    if (!parseMap(value, map)) return fail(1, "invalid --map TRACK:CHANNEL=PATCH", json);
                    options.maps.push_back(std::move(map));
                } else if (!token.empty() && token.front() == '-') {
                    return fail(1, "unknown option", json);
                } else if (!sawInput) {
                    input = filePath(token);
                    sawInput = true;
                } else {
                    return fail(1, "unexpected argument", json);
                }
            }
            if (!sawInput) return fail(1, "missing song or MIDI path", json);
            if (sub == "create") {
                nodsynth::song::CreateSongOptions created;
                created.bpm = bpm;
                created.ppq = ppq;
                if (!bars.empty() && bars.find(':') == std::string::npos) created.bars = static_cast<std::uint32_t>(std::stoul(bars));
                if (!meter.empty()) {
                    const auto slash = meter.find('/');
                    if (slash == std::string::npos) return fail(1, "meter must be N/N", json);
                    created.numerator = static_cast<std::uint8_t>(std::stoul(meter.substr(0, slash)));
                    created.denominator = static_cast<std::uint16_t>(std::stoul(meter.substr(slash + 1)));
                }
                auto song = nodsynth::song::createSong(created);
                std::string error;
                if (!nodsynth::song::saveSong(input, song, error)) return fail(4, error, json);
                if (json) std::cout << nodsynth::song::summaryJson(song, nodsynth::song::validate(song)).dump() << '\n';
                return 0;
            }
            if (sub == "import-midi") {
                if (output.empty()) return fail(1, "import-midi requires --output", json);
                const auto parsed = nodsynth::midi::parseFile(input);
                if (parsed.status != nodsynth::midi::ParseStatus::ok) return fail(1, parsed.message, json);
                auto imported = nodsynth::song::importMidi(parsed.file, options);
                if (!imported.ok) return fail(1, imported.message, json);
                imported.song.sourceMidiHash = nodsynth::song::hashFile(input);
                std::string error;
                if (!nodsynth::song::saveSong(output, imported.song, error)) return fail(4, error, json);
                if (json) std::cout << nodsynth::song::summaryJson(imported.song, nodsynth::song::validate(imported.song)).dump() << '\n';
                return 0;
            }
            if (sub == "validate") {
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                const auto validation = renderReady ? nodsynth::song::validateRenderReady(*song) : nodsynth::song::validate(*song);
                auto report = nodsynth::song::summaryJson(*song, validation);
                const auto resources = nodsynth::song::resourceDiagnostics(*song, song->baseDirectory);
                report.set("resources", resources);
                bool resourcesOk = true;
                for (const auto& diagnostic : resources.asArray())
                    if (diagnostic.find("code")->asString() != "non-portable") resourcesOk = false;
                if (json) std::cout << report.dump() << '\n';
                else if (!validation.ok) std::cerr << validation.diagnostics.front().message << '\n';
                return validation.ok && resourcesOk ? 0 : 1;
            }
            if (sub == "export-midi") {
                if (output.empty()) return fail(1, "export-midi requires --output", json);
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                if (!nodsynth::song::exportMidi(*song, output, error)) return fail(1, error, json);
                return 0;
            }
            if (sub == "apply") {
                if (commands.empty()) return fail(1, "apply requires --commands", json);
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                std::ifstream commandFile(commands, std::ios::binary);
                if (!commandFile) return fail(1, "failed to open the command file", json);
                const std::string text((std::istreambuf_iterator<char>(commandFile)), std::istreambuf_iterator<char>());
                auto batch = nodsynth::persist::Json::parse(text, error);
                if (!batch) return fail(1, error, json);
                if (!diffDetail.empty()) {
                    if (diffDetail != "notes" && diffDetail != "summary" && diffDetail != "full") return fail(1, "diff-detail must be notes, full or summary", json);
                    batch->set("diffDetail", nodsynth::persist::Json::string(diffDetail));
                }
                const auto applied = nodsynth::song::applyCommands(*song, *batch, expectRevision, dryRun);
                if (!applied.ok) return fail(classify(applied.code), applied.message, json);
                const auto destination = output.empty() ? input : output;
                if (!dryRun && !nodsynth::song::saveSong(destination, *song, error)) return fail(4, error, json);
                if (json) {
                    nodsynth::persist::Json report = nodsynth::persist::Json::object();
                    report.set("status", nodsynth::persist::Json::string("ok"));
                    report.set("revision", nodsynth::persist::Json::number(static_cast<double>(applied.revision)));
                    report.set("baseRevision", nodsynth::persist::Json::number(static_cast<double>(applied.baseRevision)));
                    report.set("unchanged", nodsynth::persist::Json::boolean(applied.unchanged));
                    report.set("noop", nodsynth::persist::Json::boolean(applied.noop));
                    report.set("requestId", nodsynth::persist::Json::string(applied.requestId));
                    report.set("diff", applied.diff);
                    report.set("idMap", applied.idMap);
                    std::cout << report.dump() << '\n';
                }
                return 0;
            }
            if (sub == "undo" || sub == "redo") {
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                const auto applied = sub == "undo" ? nodsynth::song::undoSong(*song) : nodsynth::song::redoSong(*song);
                if (!applied.ok) return fail(classify(applied.code), applied.message, json);
                const auto destination = output.empty() ? input : output;
                if (!nodsynth::song::saveSong(destination, *song, error)) return fail(4, error, json);
                if (json) {
                    nodsynth::persist::Json report = nodsynth::persist::Json::object();
                    report.set("status", nodsynth::persist::Json::string("ok"));
                    report.set("revision", nodsynth::persist::Json::number(static_cast<double>(applied.revision)));
                    report.set("baseRevision", nodsynth::persist::Json::number(static_cast<double>(applied.baseRevision)));
                    report.set("diff", applied.diff);
                    std::cout << report.dump() << '\n';
                }
                return 0;
            }
            if (sub == "query") {
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                nodsynth::song::QueryOptions query;
                query.view = view.empty() ? "legacy" : view;
                if (!trackId.empty()) query.trackId = trackId;
                query.startTick = startTick;
                query.endTick = endTick;
                query.limit = limit;
                if (!cursor.empty()) query.cursor = cursor;
                query.onsetOnly = onsetOnly;
                query.expandAutomation = expandAutomation;
                if (!bars.empty()) {
                    const auto colon = bars.find(':');
                    if (colon == std::string::npos) return fail(1, "query --bars must be START:END", json);
                    const auto startBar = static_cast<std::uint32_t>(std::stoul(bars.substr(0, colon)));
                    const auto endBar = static_cast<std::uint32_t>(std::stoul(bars.substr(colon + 1)));
                    std::uint32_t from = 0;
                    std::uint32_t to = 0;
                    if (!nodsynth::song::barsToTicks(*song, startBar, endBar, from, to, error)) return fail(1, error, json);
                    query.startTick = from;
                    query.endTick = to;
                }
                const auto report = nodsynth::song::querySong(*song, query);
                if (json) std::cout << report.dump() << '\n';
                return 0;
            }
            if (sub == "propose") {
                if (adapterExe.empty() || instruction.empty() || output.empty()) {
                    return fail(1, "propose requires --adapter, --instruction, and --output", json);
                }
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                nodsynth::song::ModelAdapter adapter;
                adapter.executable = adapterExe;
                adapter.arguments = std::move(adapterArgs);
                const auto proposal = nodsynth::song::proposeEdits(*song, instruction, adapter);
                if (!proposal.ok) return fail(classify(proposal.code), proposal.message, json);
                std::ofstream batchFile(output, std::ios::binary | std::ios::trunc);
                if (!batchFile) return fail(4, "failed to write the proposed commands", json);
                const auto text = proposal.batch.dump();
                batchFile.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!batchFile) return fail(4, "failed to write the proposed commands", json);
                if (json) {
                    nodsynth::persist::Json report = nodsynth::persist::Json::object();
                    report.set("status", nodsynth::persist::Json::string("ok"));
                    report.set("output", nodsynth::persist::Json::string(pathText(output)));
                    std::cout << report.dump() << '\n';
                }
                return 0;
            }
            usage();
            return 1;
        }
        if (command == "preset") {
            if (argc < 3) {
                usage();
                return 1;
            }
            const std::string_view sub = argv[2];
            bool json = false;
            std::string role;
            std::string id;
            std::filesystem::path output;
            std::filesystem::path root = nodsynth::song::defaultPresetsRoot();
            for (int index = 3; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (const char* value = argument(argc, argv, "--role", index)) role = value;
                else if (const char* value = argument(argc, argv, "--output", index)) output = filePath(value);
                else if (const char* value = argument(argc, argv, "--presets", index)) root = filePath(value);
                else if (!token.empty() && token.front() == '-') return fail(1, "unknown option", json);
                else if (id.empty()) id = std::string(token);
                else return fail(1, "unexpected argument", json);
            }
            if (sub == "list") {
                std::optional<std::string> filter;
                if (!role.empty()) filter = role;
                const auto presets = nodsynth::song::listPresets(root, filter);
                if (json) std::cout << nodsynth::song::listJson(presets).dump() << '\n';
                return 0;
            }
            if (sub == "inspect") {
                if (id.empty()) return fail(1, "inspect requires a preset id", json);
                const auto found = nodsynth::song::findPreset(root, id);
                if (!found) return fail(1, "preset was not found", json);
                if (json) std::cout << nodsynth::song::presetJson(*found).dump() << '\n';
                return 0;
            }
            if (sub == "audition") {
                if (id.empty() || output.empty()) return fail(1, "audition requires an id and --output", json);
                nodsynth::song::PresetAuditionOptions options;
                options.presetsRoot = root;
                const auto report = nodsynth::song::renderPresetAudition(id, output, options);
                if (!report.ok) return fail(3, report.message, json);
                if (json) {
                    auto manifest = nodsynth::persist::Json::object();
                    manifest.set("status", nodsynth::persist::Json::string("ok"));
                    manifest.set("output", nodsynth::persist::Json::string(pathText(output)));
                    manifest.set("format", nodsynth::persist::Json::string("pcm16"));
                    manifest.set("fileHash", nodsynth::persist::Json::string(nodsynth::song::hashFile(output)));
                    manifest.set("auditionStatus", nodsynth::persist::Json::string("unheard"));
                    manifest.set("peak", nodsynth::persist::Json::number(report.peak));
                    std::cout << manifest.dump() << '\n';
                }
                return 0;
            }
            usage();
            return 1;
        }
        if (command == "analyze") {
            if (argc < 3) return fail(1, "analyze requires a WAV file", false);
            bool json = false;
            std::filesystem::path input;
            for (int index = 2; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (input.empty()) input = filePath(token);
                else return fail(1, "unexpected argument", json);
            }
            const auto analysis = nodsynth::song::analyzeWav(input);
            if (json) std::cout << nodsynth::song::analysisJson(analysis).dump() << '\n';
            else if (!analysis.ok) std::cerr << analysis.message << '\n';
            return analysis.ok ? 0 : 1;
        }
        if (command == "compare") {
            if (argc < 4) return fail(1, "compare requires two WAV files", false);
            bool json = false;
            bool matchLoudness = false;
            std::filesystem::path a;
            std::filesystem::path b;
            for (int index = 2; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (token == "--match-loudness") matchLoudness = true;
                else if (a.empty()) a = token;
                else if (b.empty()) b = token;
                else return fail(1, "unexpected argument", json);
            }
            if (a.empty() || b.empty()) return fail(1, "compare requires two WAV files", json);
            nodsynth::song::CompareOptions options;
            options.matchLoudness = matchLoudness;
            const auto report = nodsynth::song::compareWav(a, b, options);
            if (json) std::cout << nodsynth::song::compareJson(report).dump() << '\n';
            else if (!report.ok) std::cerr << report.message << '\n';
            return report.ok ? 0 : 1;
        }
        if (command == "play" || command == "audition") {
            int first = command == "play" ? 2 : 3;
            const std::string sub = command == "play" ? "play" : (argc > 2 ? argv[2] : "");
            std::filesystem::path audio, records, manifestFile;
            std::string reviewer;
            bool json = false, heard = false;
            for (int index = first; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (token == "--heard") heard = true;
                else if (const char* value = argument(argc, argv, "--records", index)) records = filePath(value);
                else if (const char* value = argument(argc, argv, "--report", index)) manifestFile = filePath(value);
                else if (const char* value = argument(argc, argv, "--reviewer", index)) reviewer = value;
                else if (!token.empty() && token.front() == '-') return fail(1, "unknown audition option", json);
                else if (audio.empty()) audio = filePath(token);
                else return fail(1, "unexpected argument", json);
            }
            if (audio.empty()) return fail(1, "audio file is required", json);
            if (records.empty()) { records = audio; records += ".audition.json"; }
            std::string error;
            if (sub == "query") {
                const auto report = nodsynth::song::auditionRecords(audio, records, error);
                if (!error.empty()) return fail(1, error, json);
                std::cout << report.dump() << '\n'; return 0;
            }
            if (sub != "play" && sub != "record") return fail(1, "audition supports query or record", json);
            if (manifestFile.empty()) return fail(1, "--report is required to identify the rendered audio", json);
            if (sub == "record" && (!heard || reviewer.empty())) return fail(1, "record requires explicit --heard and --reviewer", json);
            std::ifstream input(manifestFile, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            auto manifest = nodsynth::persist::Json::parse(text, error);
            if (!manifest) return fail(1, error, json);
            if (sub == "play" && !nodsynth::song::startPlayback(audio, error)) return fail(3, error, json);
            if (!nodsynth::song::recordAudition(audio, records, *manifest, sub == "play" ? "playbackStarted" : "heard",
                reviewer.empty() ? "default-player" : reviewer, error)) return fail(1, error, json);
            const auto report = nodsynth::song::auditionRecords(audio, records, error);
            std::cout << report.dump() << '\n'; return 0;
        }
        if (command == "render") {
            std::filesystem::path input;
            std::filesystem::path output;
            std::filesystem::path stems;
            std::filesystem::path reportPath;
            std::string bars;
            bool json = false;
            bool sawInput = false;
            nodsynth::song::SongRenderOptions options;
            for (int index = 2; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (token == "--loose") options.mode = nodsynth::midi::RenderMode::loose;
                else if (token == "--no-cache") options.useCache = false;
                else if (token == "--freeze-external") options.freezeExternal = true;
                else if (const char* value = argument(argc, argv, "--format", index)) options.format = value;
                else if (const char* value = argument(argc, argv, "--pcm-overflow", index)) options.pcmOverflow = value;
                else if (const char* value = argument(argc, argv, "--preview-output", index)) options.previewOutput = filePath(value);
                else if (const char* value = argument(argc, argv, "--quality", index)) options.quality = value;
                else if (const char* value = argument(argc, argv, "--output", index)) output = filePath(value);
                else if (const char* value = argument(argc, argv, "--stems", index)) stems = filePath(value);
                else if (const char* value = argument(argc, argv, "--report", index)) reportPath = filePath(value);
                else if (const char* value = argument(argc, argv, "--cache-dir", index)) options.cacheDirectory = filePath(value);
                else if (const char* value = argument(argc, argv, "--sample-rate", index)) options.sampleRate = std::stod(value);
                else if (const char* value = argument(argc, argv, "--block-size", index)) options.blockSize = static_cast<std::uint32_t>(std::stoul(value));
                else if (const char* value = argument(argc, argv, "--tail-seconds", index)) options.tailSeconds = std::stod(value);
                else if (const char* value = argument(argc, argv, "--max-tail-seconds", index)) options.maxTailSeconds = std::stod(value);
                else if (const char* value = argument(argc, argv, "--tail-threshold", index)) options.tailThreshold = std::stod(value);
                else if (const char* value = argument(argc, argv, "--start-tick", index)) {
                    options.previewStartTick = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--end-tick", index)) {
                    options.previewEndTick = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--bars", index)) {
                    bars = value;
                }
                else if (const char* value = argument(argc, argv, "--fluidsynth", index)) {
                    nodsynth::song::SongRenderOptions::ExternalTool tool;
                    tool.adapter = "fluidsynth";
                    tool.executable = filePath(value);
                    options.tools.push_back(std::move(tool));
                } else if (const char* value = argument(argc, argv, "--vst3-worker", index)) {
                    nodsynth::song::SongRenderOptions::ExternalTool tool;
                    tool.adapter = "vst3";
                    tool.executable = filePath(value);
                    options.tools.push_back(std::move(tool));
                }
                else if (!token.empty() && token.front() == '-') return fail(1, "unknown option", json);
                else if (!sawInput) {
                    input = filePath(token);
                    sawInput = true;
                } else return fail(1, "unexpected argument", json);
            }
            if (!sawInput || output.empty()) return fail(1, "render requires a song and --output", json);
            std::string error;
            auto song = nodsynth::song::loadSong(input, error);
            if (!song) return fail(1, error, json);
            options.baseDirectory = input.parent_path();
            options.songHash = nodsynth::song::hashFile(input);
            if (!bars.empty()) {
                const auto colon = bars.find(':');
                if (colon == std::string::npos) return fail(1, "render --bars must be START:END", json);
                const auto startBar = static_cast<std::uint32_t>(std::stoul(bars.substr(0, colon)));
                const auto endBar = static_cast<std::uint32_t>(std::stoul(bars.substr(colon + 1)));
                std::uint32_t from = 0;
                std::uint32_t to = 0;
                if (!nodsynth::song::barsToTicks(*song, startBar, endBar, from, to, error)) return fail(1, error, json);
                options.previewStartTick = from;
                options.previewEndTick = to;
            }
            if (options.cacheDirectory.empty()) options.cacheDirectory = output.parent_path() / ".nod-cache";
            const auto report = nodsynth::song::renderSong(*song, options, output, stems);
            const auto jsonText = nodsynth::song::songReportJson(report).dump();
            if (!reportPath.empty()) {
                std::ofstream reportFile(reportPath, std::ios::binary | std::ios::trunc);
                if (!reportFile) return fail(4, "failed to write the report", json);
                reportFile << jsonText << '\n';
            }
            if (json) std::cout << jsonText << '\n';
            else if (!report.ok) std::cerr << report.message << '\n';
            return report.ok ? 0 : classify(report.code);
        }
    } catch (const std::exception& error) {
        return fail(1, error.what(), false);
    }
    usage();
    return 1;
}
