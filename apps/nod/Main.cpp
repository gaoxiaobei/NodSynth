#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/ModelClient.h>
#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/SongRenderer.h>

namespace {
void usage() {
    std::cerr << "usage: nod song import-midi FILE --output FILE [--map TRACK:CHANNEL=PATCH]...\n"
              << "       nod song validate FILE [--json]\n"
              << "       nod song apply FILE --commands FILE [--expect-revision N] [--output FILE]\n"
              << "       nod song undo FILE [--output FILE]\n"
              << "       nod song redo FILE [--output FILE]\n"
              << "       nod song query FILE [--start-tick N] [--end-tick N] [--json]\n"
              << "       nod song propose FILE --adapter EXE --instruction TEXT --output FILE [--adapter-arg ARG]... [--json]\n"
              << "       nod song export-midi FILE --output FILE\n"
              << "       nod analyze FILE [--json]\n"
              << "       nod render FILE --output FILE [--stems DIR] [--report FILE]\n"
              << "                 [--sample-rate 48000] [--block-size 128] [--tail-seconds 2]\n"
              << "                 [--start-tick N] [--end-tick N] [--tail-threshold AMPLITUDE] [--max-tail-seconds 8] [--loose] [--json]\n"
              << "                 [--fluidsynth EXE] [--vst3-worker EXE]\n"
              << "Import creates one track per source track and channel. Patches are assigned only by --map.\n";
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
    if (code == "revision-conflict") return 5;
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
            bool json = false;
            bool sawInput = false;
            nodsynth::song::ImportOptions options;
            for (int index = 3; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") {
                    json = true;
                } else if (const char* value = argument(argc, argv, "--output", index)) {
                    output = value;
                } else if (const char* value = argument(argc, argv, "--commands", index)) {
                    commands = value;
                } else if (const char* value = argument(argc, argv, "--adapter", index)) {
                    adapterExe = value;
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
                } else if (const char* value = argument(argc, argv, "--map", index)) {
                    nodsynth::song::StreamMap map;
                    if (!parseMap(value, map)) return fail(1, "invalid --map TRACK:CHANNEL=PATCH", json);
                    options.maps.push_back(std::move(map));
                } else if (!token.empty() && token.front() == '-') {
                    return fail(1, "unknown option", json);
                } else if (!sawInput) {
                    input = token;
                    sawInput = true;
                } else {
                    return fail(1, "unexpected argument", json);
                }
            }
            if (!sawInput) return fail(1, "missing song or MIDI path", json);
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
                const auto validation = nodsynth::song::validate(*song);
                if (json) std::cout << nodsynth::song::summaryJson(*song, validation).dump() << '\n';
                else if (!validation.ok) std::cerr << validation.diagnostics.front().message << '\n';
                return validation.ok ? 0 : 1;
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
                const auto applied = nodsynth::song::applyCommands(*song, *batch, expectRevision);
                if (!applied.ok) return fail(classify(applied.code), applied.message, json);
                const auto destination = output.empty() ? input : output;
                if (!nodsynth::song::saveSong(destination, *song, error)) return fail(4, error, json);
                if (json) {
                    nodsynth::persist::Json report = nodsynth::persist::Json::object();
                    report.set("status", nodsynth::persist::Json::string("ok"));
                    report.set("revision", nodsynth::persist::Json::number(static_cast<double>(applied.revision)));
                    report.set("unchanged", nodsynth::persist::Json::boolean(applied.unchanged));
                    report.set("diff", applied.diff);
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
                    std::cout << report.dump() << '\n';
                }
                return 0;
            }
            if (sub == "query") {
                std::string error;
                auto song = nodsynth::song::loadSong(input, error);
                if (!song) return fail(1, error, json);
                const auto report = nodsynth::song::querySong(*song, startTick, endTick);
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
                    report.set("output", nodsynth::persist::Json::string(output.string()));
                    std::cout << report.dump() << '\n';
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
                else if (input.empty()) input = token;
                else return fail(1, "unexpected argument", json);
            }
            const auto analysis = nodsynth::song::analyzeWav(input);
            if (json) std::cout << nodsynth::song::analysisJson(analysis).dump() << '\n';
            else if (!analysis.ok) std::cerr << analysis.message << '\n';
            return analysis.ok ? 0 : 1;
        }
        if (command == "render") {
            std::filesystem::path input;
            std::filesystem::path output;
            std::filesystem::path stems;
            std::filesystem::path reportPath;
            bool json = false;
            bool sawInput = false;
            nodsynth::song::SongRenderOptions options;
            for (int index = 2; index < argc; ++index) {
                const std::string_view token = argv[index];
                if (token == "--json") json = true;
                else if (token == "--loose") options.mode = nodsynth::midi::RenderMode::loose;
                else if (const char* value = argument(argc, argv, "--output", index)) output = value;
                else if (const char* value = argument(argc, argv, "--stems", index)) stems = value;
                else if (const char* value = argument(argc, argv, "--report", index)) reportPath = value;
                else if (const char* value = argument(argc, argv, "--sample-rate", index)) options.sampleRate = std::stod(value);
                else if (const char* value = argument(argc, argv, "--block-size", index)) options.blockSize = static_cast<std::uint32_t>(std::stoul(value));
                else if (const char* value = argument(argc, argv, "--tail-seconds", index)) options.tailSeconds = std::stod(value);
                else if (const char* value = argument(argc, argv, "--max-tail-seconds", index)) options.maxTailSeconds = std::stod(value);
                else if (const char* value = argument(argc, argv, "--tail-threshold", index)) options.tailThreshold = std::stod(value);
                else if (const char* value = argument(argc, argv, "--start-tick", index)) {
                    options.previewStartTick = static_cast<std::uint32_t>(std::stoul(value));
                } else if (const char* value = argument(argc, argv, "--end-tick", index)) {
                    options.previewEndTick = static_cast<std::uint32_t>(std::stoul(value));
                }
                else if (const char* value = argument(argc, argv, "--fluidsynth", index)) {
                    nodsynth::song::SongRenderOptions::ExternalTool tool;
                    tool.adapter = "fluidsynth";
                    tool.executable = value;
                    options.tools.push_back(std::move(tool));
                } else if (const char* value = argument(argc, argv, "--vst3-worker", index)) {
                    nodsynth::song::SongRenderOptions::ExternalTool tool;
                    tool.adapter = "vst3";
                    tool.executable = value;
                    options.tools.push_back(std::move(tool));
                }
                else if (!token.empty() && token.front() == '-') return fail(1, "unknown option", json);
                else if (!sawInput) {
                    input = token;
                    sawInput = true;
                } else return fail(1, "unexpected argument", json);
            }
            if (!sawInput || output.empty()) return fail(1, "render requires a song and --output", json);
            std::string error;
            auto song = nodsynth::song::loadSong(input, error);
            if (!song) return fail(1, error, json);
            options.baseDirectory = input.parent_path();
            options.songHash = nodsynth::song::hashFile(input);
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
