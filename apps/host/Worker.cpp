#include <nodsynth/host/Vst3Host.h>
#include <nodsynth/midi/Smf.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int fail(const std::string& message)
{
    std::cerr << message << '\n';
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--crash") std::abort();
    if (argc < 8) return fail("usage: nod_vst3_worker plugin class midi wav rate block tail-seconds");
    const std::filesystem::path plugin = argv[1];
    const std::string className = std::string(argv[2]) == "-" ? std::string{} : argv[2];
    const std::filesystem::path midiPath = argv[3];
    const std::filesystem::path wavPath = argv[4];
    const auto rate = static_cast<std::uint32_t>(std::stoul(argv[5]));
    const auto block = static_cast<std::uint32_t>(std::stoul(argv[6]));
    const auto tailSeconds = std::stod(argv[7]);
    if (rate == 0 || block == 0) return fail("sample rate and block size must be positive");

    const auto parsed = nodsynth::midi::parseFile(midiPath);
    if (parsed.status != nodsynth::midi::ParseStatus::ok) return fail(parsed.message.empty() ? "failed to read MIDI" : parsed.message);
    const auto scheduled = nodsynth::midi::schedule(parsed.file, rate, block);
    if (scheduled.status != nodsynth::midi::ParseStatus::ok) return fail(scheduled.message.empty() ? "failed to schedule MIDI" : scheduled.message);

    nodsynth::host::Vst3Plugin instrument;
    std::string error;
    if (!instrument.open(plugin, error, className)) return fail(error.empty() ? "failed to open the plugin" : error);
    if (!instrument.activate(rate, block, error)) return fail(error.empty() ? "failed to activate the plugin" : error);

    struct ResolvedPoint {
        std::uint32_t sample{0};
        std::uint32_t id{0};
        double value{0};
    };
    std::vector<ResolvedPoint> automation;
    if (argc >= 9 && std::string(argv[8]) != "-") {
        std::ifstream input(std::filesystem::path(argv[8]), std::ios::binary);
        if (!input) return fail("failed to open parameter automation");
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto document = nodsynth::persist::Json::parse(text, error);
        const auto* points = document ? document->find("points") : nullptr;
        if (!document || points == nullptr || points->kind() != nodsynth::persist::Json::Kind::array) {
            return fail(error.empty() ? "parameter automation is invalid" : error);
        }
        const auto listed = instrument.parameters();
        for (const auto& point : points->asArray()) {
            const auto* name = point.find("parameter");
            if (name == nullptr || name->kind() != nodsynth::persist::Json::Kind::string) return fail("a parameter point has no name");
            const auto found = std::find_if(listed.begin(), listed.end(), [&](const nodsynth::host::Vst3Parameter& parameter) {
                return parameter.title == name->asString();
            });
            if (found == listed.end()) return fail("the plugin has no parameter named " + name->asString());
            ResolvedPoint resolved;
            resolved.sample = static_cast<std::uint32_t>(std::max(0.0, point.find("sample") == nullptr ? 0.0 : point.find("sample")->asNumber()));
            resolved.id = found->id;
            resolved.value = point.find("value") == nullptr ? 0.0 : point.find("value")->asNumber();
            automation.push_back(resolved);
        }
        std::stable_sort(automation.begin(), automation.end(), [](const ResolvedPoint& left, const ResolvedPoint& right) {
            return left.sample < right.sample;
        });
    }

    std::int64_t last = 0;
    std::vector<nodsynth::host::Vst3Midi> events;
    events.reserve(scheduled.events.size());
    for (const auto& event : scheduled.events) {
        const bool note = event.source.kind == nodsynth::midi::EventKind::noteOn || event.source.kind == nodsynth::midi::EventKind::noteOff;
        if (!note) continue;
        nodsynth::host::Vst3Midi midi;
        midi.noteOn = event.source.kind == nodsynth::midi::EventKind::noteOn && event.source.data2 > 0;
        midi.channel = event.source.channel;
        midi.pitch = event.source.data1;
        midi.velocity = midi.noteOn ? event.source.data2 : 0;
        events.push_back(midi);
        events.back().sampleOffset = static_cast<std::uint32_t>(std::max<std::int64_t>(event.absoluteSample, 0));
        last = std::max(last, event.absoluteSample);
    }
    std::stable_sort(events.begin(), events.end(), [](const nodsynth::host::Vst3Midi& left, const nodsynth::host::Vst3Midi& right) {
        return left.sampleOffset < right.sampleOffset;
    });
    const auto latency = std::max(0, instrument.info().latencySamples);
    const auto tail = static_cast<std::int64_t>(std::llround(std::max(0.0, tailSeconds) * rate));
    const auto total = static_cast<std::uint64_t>(last + tail + latency + block);
    std::vector<float> interleaved(static_cast<std::size_t>(total) * 2, 0.f);
    std::vector<float> left(block, 0.f);
    std::vector<float> right(block, 0.f);
    std::size_t cursor = 0;
    std::size_t paramCursor = 0;
    for (std::uint64_t rendered = 0; rendered < total; rendered += block) {
        std::vector<nodsynth::host::Vst3Midi> blockEvents;
        const auto blockEnd = rendered + block;
        while (cursor < events.size() && events[cursor].sampleOffset < blockEnd) {
            auto midi = events[cursor];
            midi.sampleOffset = static_cast<std::uint32_t>(midi.sampleOffset - rendered);
            blockEvents.push_back(midi);
            ++cursor;
        }
        std::vector<nodsynth::host::Vst3ParamPoint> blockParameters;
        while (paramCursor < automation.size() && automation[paramCursor].sample < rendered + block) {
            if (automation[paramCursor].sample >= rendered) {
                nodsynth::host::Vst3ParamPoint point;
                point.sampleOffset = static_cast<std::uint32_t>(automation[paramCursor].sample - rendered);
                point.id = automation[paramCursor].id;
                point.value = automation[paramCursor].value;
                blockParameters.push_back(point);
            }
            ++paramCursor;
        }
        if (!instrument.process(left.data(), right.data(), block, blockEvents, error, nullptr, nullptr, blockParameters.empty() ? nullptr : &blockParameters)) {
            return fail(error.empty() ? "plugin process failed" : error);
        }
        for (std::uint32_t frame = 0; frame < block && rendered + frame < total; ++frame) {
            interleaved[static_cast<std::size_t>(rendered + frame) * 2] = left[frame];
            interleaved[static_cast<std::size_t>(rendered + frame) * 2 + 1] = right[frame];
        }
    }

    nodsynth::runtime::WavData wav;
    wav.sampleRate = rate;
    wav.channels = 2;
    wav.interleaved = std::move(interleaved);
    if (!nodsynth::runtime::writeWav(wavPath, wav, error)) return fail(error.empty() ? "failed to write audio" : error);
    std::ofstream latencyFile(wavPath.string() + ".latency", std::ios::trunc);
    latencyFile << latency;
    if (!latencyFile) return fail("failed to write the plugin latency");
    return 0;
}
