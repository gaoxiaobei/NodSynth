#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nodsynth/midi/Smf.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/render/OfflineRenderer.h>
#include <nodsynth/runtime/WavFile.h>

using namespace nodsynth;

namespace {
void writeVarlen(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t bytes[4]{};
    int count = 0;
    bytes[count++] = static_cast<std::uint8_t>(value & 0x7f);
    value >>= 7;
    while (value != 0 && count < 4) {
        bytes[count++] = static_cast<std::uint8_t>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    while (count > 0) out.push_back(bytes[--count]);
}

void write16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

class SmfBuilder {
public:
    std::vector<std::vector<std::uint8_t>> tracks{1};
    std::vector<std::uint32_t> endDeltas{0};
    std::uint16_t format{0};

    void event(std::size_t track, std::uint32_t delta, std::initializer_list<std::uint8_t> bytes) {
        writeVarlen(tracks.at(track), delta);
        tracks.at(track).insert(tracks.at(track).end(), bytes);
    }

    void setEnd(std::size_t track, std::uint32_t delta) {
        if (endDeltas.size() <= track) endDeltas.resize(track + 1);
        endDeltas[track] = delta;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
        write32(out, 6);
        write16(out, format);
        write16(out, static_cast<std::uint16_t>(tracks.size()));
        write16(out, 480);
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            out.insert(out.end(), {'M', 'T', 'r', 'k'});
            auto body = tracks[index];
            writeVarlen(body, index < endDeltas.size() ? endDeltas[index] : 0);
            body.insert(body.end(), {0xff, 0x2f, 0x00});
            write32(out, static_cast<std::uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
        }
        return out;
    }
};

midi::File loadMidi(const SmfBuilder& builder) {
    const auto bytes = builder.build();
    const auto parsed = midi::parse(bytes.data(), bytes.size());
    REQUIRE(parsed.status == midi::ParseStatus::ok);
    return parsed.file;
}

render::RenderReport renderFile(const SmfBuilder& builder, const std::filesystem::path& path, render::RenderOptions options = {}) {
    return render::renderMidi(nodes::sinePatch(), loadMidi(builder), options, path);
}

double rms(const runtime::WavData& wav, std::uint32_t start, std::uint32_t end) {
    double sum = 0.0;
    std::uint32_t count = 0;
    for (std::uint32_t frame = start; frame < end && frame * wav.channels < wav.interleaved.size(); ++frame) {
        const float sample = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels];
        sum += static_cast<double>(sample) * sample;
        ++count;
    }
    return count == 0 ? 0.0 : std::sqrt(sum / count);
}

double frequency(const runtime::WavData& wav, std::uint32_t start, std::uint32_t end) {
    int crossings = 0;
    float previous = 0.f;
    bool have = false;
    for (std::uint32_t frame = start; frame < end && frame * wav.channels < wav.interleaved.size(); ++frame) {
        const float sample = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels];
        if (have && previous <= 0.f && sample > 0.f) ++crossings;
        previous = sample;
        have = true;
    }
    const double seconds = static_cast<double>(end - start) / wav.sampleRate;
    return seconds <= 0.0 ? 0.0 : static_cast<double>(crossings) / seconds;
}

std::filesystem::path tempWav(const char* name) { return std::filesystem::temp_directory_path() / name; }
} // namespace

TEST_CASE("a saved filter patch renders a MIDI note") {
    const auto patch = tempWav("nodsynth-filter.nodsynth.json");
    const auto wav = tempWav("nodsynth-filter.wav");
    auto document = persist::projectFromGraph(nodes::filterPatch());
    std::string error;
    REQUIRE(persist::saveProject(patch, document, error));
    const auto graph = render::loadPatch(patch, error);
    REQUIRE(graph.has_value());
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 100});
    builder.setEnd(0, 480);
    render::RenderOptions options;
    options.tailSeconds = 0.2;
    const auto report = render::renderMidi(*graph, loadMidi(builder), options, wav);
    REQUIRE(report.ok);
    runtime::WavData data;
    REQUIRE(runtime::readWav(wav, data, error));
    REQUIRE(report.peak > 0.001f);
    std::filesystem::remove(patch);
    std::filesystem::remove(wav);
}

TEST_CASE("sustain holds a note and all-notes-off and all-sound-off release it") {
    const auto heldPath = tempWav("nodsynth-sustain.wav");
    const auto releasedPath = tempWav("nodsynth-released.wav");
    SmfBuilder held;
    held.event(0, 0, {0x90, 60, 110});
    held.event(0, 0, {0xb0, 64, 127});
    held.event(0, 192, {0x80, 60, 0});
    held.event(0, 768, {0xb0, 64, 0});
    SmfBuilder released;
    released.event(0, 0, {0x90, 60, 110});
    released.event(0, 192, {0x80, 60, 0});
    released.setEnd(0, 960);
    render::RenderOptions options;
    options.tailSeconds = 0.1;
    REQUIRE(renderFile(held, heldPath, options).ok);
    REQUIRE(renderFile(released, releasedPath, options).ok);
    std::string error;
    runtime::WavData heldWav;
    runtime::WavData releasedWav;
    REQUIRE(runtime::readWav(heldPath, heldWav, error));
    REQUIRE(runtime::readWav(releasedPath, releasedWav, error));
    REQUIRE(rms(heldWav, 22000, 26000) > 0.02);
    REQUIRE(rms(releasedWav, 22000, 26000) < 0.005);

    const auto notesOffPath = tempWav("nodsynth-cc123.wav");
    const auto soundOffPath = tempWav("nodsynth-cc120.wav");
    SmfBuilder notesOff;
    notesOff.event(0, 0, {0x90, 64, 110});
    notesOff.event(0, 192, {0xb0, 123, 0});
    notesOff.setEnd(0, 960);
    SmfBuilder soundOff;
    soundOff.event(0, 0, {0x90, 64, 110});
    soundOff.event(0, 192, {0xb0, 120, 0});
    soundOff.setEnd(0, 960);
    REQUIRE(renderFile(notesOff, notesOffPath, options).ok);
    REQUIRE(renderFile(soundOff, soundOffPath, options).ok);
    runtime::WavData notesOffWav;
    runtime::WavData soundOffWav;
    REQUIRE(runtime::readWav(notesOffPath, notesOffWav, error));
    REQUIRE(runtime::readWav(soundOffPath, soundOffWav, error));
    REQUIRE(rms(notesOffWav, 1000, 3000) > 0.02);
    REQUIRE(rms(notesOffWav, 22000, 26000) < 0.005);
    REQUIRE(rms(soundOffWav, 1000, 3000) > 0.02);
    REQUIRE(rms(soundOffWav, 22000, 26000) < 0.005);
    std::filesystem::remove(heldPath);
    std::filesystem::remove(releasedPath);
    std::filesystem::remove(notesOffPath);
    std::filesystem::remove(soundOffPath);
}

TEST_CASE("pitch bend raises the rendered frequency") {
    const auto plainPath = tempWav("nodsynth-plain.wav");
    const auto bentPath = tempWav("nodsynth-bent.wav");
    SmfBuilder plain;
    plain.event(0, 0, {0x90, 69, 100});
    plain.event(0, 0, {0xe0, 0x00, 0x40});
    plain.setEnd(0, 960);
    SmfBuilder bent;
    bent.event(0, 0, {0x90, 69, 100});
    bent.event(0, 0, {0xe0, 0x7f, 0x7f});
    bent.setEnd(0, 960);
    render::RenderOptions options;
    options.tailSeconds = 0.05;
    REQUIRE(renderFile(plain, plainPath, options).ok);
    REQUIRE(renderFile(bent, bentPath, options).ok);
    std::string error;
    runtime::WavData plainWav;
    runtime::WavData bentWav;
    REQUIRE(runtime::readWav(plainPath, plainWav, error));
    REQUIRE(runtime::readWav(bentPath, bentWav, error));
    const auto plainHz = frequency(plainWav, 14400, 38400);
    const auto bentHz = frequency(bentWav, 14400, 38400);
    REQUIRE(plainHz == Catch::Approx(440.0).margin(8.0));
    REQUIRE(bentHz == Catch::Approx(493.88).margin(8.0));
    std::filesystem::remove(plainPath);
    std::filesystem::remove(bentPath);
}

TEST_CASE("the tail releases a held note instead of leaving it hanging") {
    const auto path = tempWav("nodsynth-tail.wav");
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 120});
    builder.setEnd(0, 480);
    render::RenderOptions options;
    options.tailSeconds = 1.0;
    const auto report = renderFile(builder, path, options);
    REQUIRE(report.ok);
    REQUIRE(report.tailFrames == 48000);
    std::string error;
    runtime::WavData wav;
    REQUIRE(runtime::readWav(path, wav, error));
    REQUIRE(rms(wav, 4000, 8000) > 0.02);
    REQUIRE(rms(wav, static_cast<std::uint32_t>(wav.interleaved.size() / wav.channels) - 8000,
                static_cast<std::uint32_t>(wav.interleaved.size() / wav.channels)) < 0.005);
    std::filesystem::remove(path);
}

TEST_CASE("event placement stays within one sample across block sizes") {
    SmfBuilder builder;
    builder.event(0, 3, {0x90, 67, 100});
    builder.setEnd(0, 96);
    render::RenderOptions small;
    small.blockSize = 64;
    small.tailSeconds = 0.05;
    render::RenderOptions large = small;
    large.blockSize = 128;
    const auto smallPath = tempWav("nodsynth-block64.wav");
    const auto largePath = tempWav("nodsynth-block128.wav");
    REQUIRE(renderFile(builder, smallPath, small).ok);
    REQUIRE(renderFile(builder, largePath, large).ok);
    std::string error;
    runtime::WavData smallWav;
    runtime::WavData largeWav;
    REQUIRE(runtime::readWav(smallPath, smallWav, error));
    REQUIRE(runtime::readWav(largePath, largeWav, error));
    const auto onset = [](const runtime::WavData& wav) {
        for (std::uint32_t frame = 0; frame < wav.interleaved.size() / wav.channels; ++frame) {
            if (std::fabs(wav.interleaved[static_cast<std::size_t>(frame) * wav.channels]) > 0.0001f) return frame;
        }
        return 0u;
    };
    const auto smallOnset = onset(smallWav);
    const auto largeOnset = onset(largeWav);
    REQUIRE(smallOnset > 0);
    REQUIRE(std::abs(static_cast<int>(smallOnset) - static_cast<int>(largeOnset)) <= 1);
    std::filesystem::remove(smallPath);
    std::filesystem::remove(largePath);
}

TEST_CASE("strict mode and dense blocks fail without pretending to succeed") {
    const auto densePath = tempWav("nodsynth-dense.wav");
    std::filesystem::remove(densePath);
    SmfBuilder dense;
    for (int note = 0; note < 3000; ++note) dense.event(0, 0, {0x90, static_cast<std::uint8_t>(note % 128), 20});
    const auto denseReport = renderFile(dense, densePath);
    REQUIRE_FALSE(denseReport.ok);
    REQUIRE(denseReport.code == "event-density");
    REQUIRE_FALSE(std::filesystem::exists(densePath));

    const auto programPath = tempWav("nodsynth-program.wav");
    std::filesystem::remove(programPath);
    SmfBuilder program;
    program.event(0, 0, {0xc0, 4});
    program.event(0, 0, {0x90, 60, 100});
    const auto strict = renderFile(program, programPath);
    REQUIRE_FALSE(strict.ok);
    REQUIRE(strict.code == "unsupported-event");
    REQUIRE_FALSE(std::filesystem::exists(programPath));
    render::RenderOptions loose;
    loose.mode = midi::RenderMode::loose;
    loose.tailSeconds = 0.05;
    const auto looseReport = renderFile(program, programPath, loose);
    REQUIRE(looseReport.ok);
    REQUIRE_FALSE(looseReport.diagnostics.empty());
    std::filesystem::remove(programPath);

    SmfBuilder multi;
    multi.event(0, 0, {0x90, 60, 100});
    multi.event(0, 0, {0x91, 64, 100});
    const auto ambiguous = renderFile(multi, tempWav("nodsynth-multi.wav"));
    REQUIRE(ambiguous.code == "stream-selection");
    render::RenderOptions channel;
    channel.selection.channel = 1;
    channel.tailSeconds = 0.05;
    const auto selectedPath = tempWav("nodsynth-channel.wav");
    const auto selected = renderFile(multi, selectedPath, channel);
    REQUIRE(selected.ok);
    REQUIRE(selected.peak > 0.001f);
    std::filesystem::remove(selectedPath);
}

TEST_CASE("offline rendering streams past the old 30 second cap") {
    const auto path = tempWav("nodsynth-long.wav");
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 40});
    builder.setEnd(0, 48);
    render::RenderOptions options;
    options.blockSize = 8192;
    options.tailSeconds = 31.0;
    const auto report = renderFile(builder, path, options);
    REQUIRE(report.ok);
    REQUIRE(report.frames > 30ull * 48000ull);
    const auto bytes = std::filesystem::file_size(path);
    REQUIRE(bytes == 44 + report.frames * 8);
    std::filesystem::remove(path);
}

TEST_CASE("inspect reports streams and tempo without rendering") {
    SmfBuilder builder;
    builder.format = 1;
    builder.tracks.resize(2);
    builder.event(0, 0, {0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
    builder.event(1, 0, {0x90, 62, 90});
    const auto file = loadMidi(builder);
    const auto json = render::inspectMidi(file);
    REQUIRE(json.find("format")->asNumber() == 1);
    REQUIRE(json.find("explicitStreamRequired")->asBool() == false);
    REQUIRE(json.find("tempo")->asArray().size() == 1);
    REQUIRE(json.find("streams")->asArray().size() == 1);
}

TEST_CASE("threshold tail ends at silence and reports when the cap cuts it") {
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 120});
    builder.setEnd(0, 96);
    render::RenderOptions quiet;
    quiet.tailMode = render::TailMode::threshold;
    quiet.tailThreshold = 0.0001;
    quiet.maxTailSeconds = 2.0;
    const auto quietPath = tempWav("nodsynth-threshold.wav");
    const auto quietReport = renderFile(builder, quietPath, quiet);
    REQUIRE(quietReport.ok);
    REQUIRE_FALSE(quietReport.tailTruncated);
    REQUIRE(quietReport.tailFrames > 4800);
    REQUIRE(quietReport.tailFrames < 2ull * 48000ull);
    std::filesystem::remove(quietPath);

    render::RenderOptions capped = quiet;
    capped.maxTailSeconds = 0.05;
    const auto cappedPath = tempWav("nodsynth-truncated.wav");
    const auto cappedReport = renderFile(builder, cappedPath, capped);
    REQUIRE(cappedReport.ok);
    REQUIRE(cappedReport.tailTruncated);
    REQUIRE(cappedReport.tailFrames == static_cast<std::uint64_t>(std::llround(0.05 * 48000.0)));
    std::filesystem::remove(cappedPath);
}

TEST_CASE("nod_render exit codes separate usage, missing input, and rejected MIDI") {
    const std::filesystem::path exe = NOD_RENDER_EXECUTABLE;
    const auto sink =
#if defined(_WIN32)
        " > nul 2>&1";
#else
        " > /dev/null 2>&1";
#endif
    const auto run = [&](const std::string& args) {
        std::string command = "\"" + exe.generic_string() + "\" " + args + sink;
#if defined(_WIN32)
        command = "\"" + command + "\"";
#endif
        const auto status = std::system(command.c_str());
#if defined(_WIN32)
        return status;
#else
        return WEXITSTATUS(status);
#endif
    };
    REQUIRE(run("--missing") == 1);

    const auto midiPath = tempWav("nodsynth-cli.mid");
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 100});
    builder.event(0, 0, {0xc0, 1});
    const auto bytes = builder.build();
    {
        std::ofstream out(midiPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto midi = "\"" + midiPath.generic_string() + "\"";
    const auto wav = "\"" + tempWav("nodsynth-cli.wav").generic_string() + "\"";
    const auto patch = "\"" + tempWav("nodsynth-missing-patch.json").generic_string() + "\"";
    REQUIRE(run("--midi " + midi + " --inspect") == 0);
    REQUIRE(run("--midi " + midi + " --validate") == 3);
    const auto playablePath = tempWav("nodsynth-cli-playable.mid");
    SmfBuilder playable;
    playable.event(0, 0, {0x90, 60, 80});
    playable.setEnd(0, 48);
    const auto playableBytes = playable.build();
    {
        std::ofstream out(playablePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(playableBytes.data()), static_cast<std::streamsize>(playableBytes.size()));
    }
    const auto playableArg = "\"" + playablePath.generic_string() + "\"";
    const auto blocked = tempWav("nodsynth-cli-blocked");
    std::filesystem::remove_all(blocked);
    std::filesystem::create_directory(blocked);
    {
        std::ofstream marker(blocked / "keep");
        marker.put('x');
    }
    REQUIRE(run("--midi " + playableArg + " --output \"" + blocked.generic_string() + "\" --tail-seconds 0.05") == 4);
    REQUIRE(run("--midi " + midi + " --patch " + patch + " --output " + wav) == 2);
    std::filesystem::remove(midiPath);
    std::filesystem::remove(playablePath);
    std::filesystem::remove_all(blocked);
}
