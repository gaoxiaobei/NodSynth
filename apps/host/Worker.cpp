#include <nodsynth/host/Vst3Host.h>
#include <nodsynth/host/EffectWorker.h>
#include <nodsynth/midi/Smf.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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
    if(argc==3 && std::string(argv[1])=="--effect-worker") return nodsynth::host::runEffectWorker(argv[2]);
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
    if(argc>=10 && std::string(argv[9])!="-") {
        std::ifstream input(std::filesystem::path(argv[9]),std::ios::binary|std::ios::ate);
        if(!input || input.tellg()<0 || input.tellg()>16*1024*1024) return fail("missing or oversized VST3 instrument state");
        std::vector<char> state(static_cast<std::size_t>(input.tellg()));input.seekg(0);input.read(state.data(),state.size());
        if(!input || !instrument.restoreState(state,error)) return fail(error.empty()?"failed to restore instrument state":error);
    }
    if (!instrument.activate(rate, block, error)) return fail(error.empty() ? "failed to activate the plugin" : error);

    struct ResolvedPoint {
        std::uint32_t sample{0};
        std::uint32_t id{0};
        double value{0};
    };
    std::map<std::uint32_t, std::vector<ResolvedPoint>> automation;
    std::map<std::uint32_t, double> baseValues;
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
            std::optional<std::uint32_t> stableId;
            const auto& address = name->asString();
            if (address.starts_with("vst3:")) {
                std::uint32_t id = 0;
                const auto parsedId = std::from_chars(address.data() + 5, address.data() + address.size(), id);
                if (parsedId.ec != std::errc{} || parsedId.ptr != address.data() + address.size()) return fail("invalid VST3 stable parameter ID");
                stableId = id;
            }
            const auto matches = [&](const auto& parameter) { return stableId ? parameter.id == *stableId : parameter.title == address; };
            if (std::count_if(listed.begin(), listed.end(), matches) != 1) return fail("parameter address is absent or ambiguous: " + address);
            const auto found = std::find_if(listed.begin(), listed.end(), matches);
            if (!found->canAutomate) return fail("parameter cannot be automated: " + address);
            ResolvedPoint resolved;
            const auto* time = point.find("sample"); const auto* value = point.find("value");
            if (!time || !value || time->kind() != nodsynth::persist::Json::Kind::number || value->kind() != nodsynth::persist::Json::Kind::number ||
                !std::isfinite(time->asNumber()) || time->asNumber() < 0 || time->asNumber() > UINT32_MAX || std::floor(time->asNumber()) != time->asNumber() ||
                !std::isfinite(value->asNumber()) || value->asNumber() < 0 || value->asNumber() > 1) return fail("invalid normalized parameter point");
            resolved.sample = static_cast<std::uint32_t>(time->asNumber());
            resolved.id = found->id;
            resolved.value = value->asNumber();
            automation[resolved.id].push_back(resolved);
            baseValues[resolved.id] = found->value;
        }
        if (automation.size() > 64) return fail("at most 64 parameter lanes are supported");
        for (auto& [id, lane] : automation) {
            std::stable_sort(lane.begin(), lane.end(), [](const auto& a, const auto& b) { return a.sample < b.sample; });
            for (std::size_t i = 1; i < lane.size(); ++i)
                if (lane[i].sample <= lane[i - 1].sample) return fail("parameter samples must be strictly increasing");
        }
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
    std::vector<nodsynth::host::Vst3ParamPoint> blockParameters;
    blockParameters.reserve(static_cast<std::size_t>(block) * automation.size());
    for (std::uint64_t rendered = 0; rendered < total; rendered += block) {
        std::vector<nodsynth::host::Vst3Midi> blockEvents;
        const auto blockEnd = rendered + block;
        while (cursor < events.size() && events[cursor].sampleOffset < blockEnd) {
            auto midi = events[cursor];
            midi.sampleOffset = static_cast<std::uint32_t>(midi.sampleOffset - rendered);
            blockEvents.push_back(midi);
            ++cursor;
        }
        blockParameters.clear();
        for (std::uint32_t frame = 0; frame < block; ++frame) {
            const auto sample = rendered + frame;
            for (const auto& [id, lane] : automation) {
                double value = baseValues.at(id);
                if (sample >= lane.front().sample) {
                    const auto upper = std::upper_bound(lane.begin(), lane.end(), sample,
                        [](auto time, const auto& point) { return time < point.sample; });
                    const auto& lower = *(upper - 1); value = lower.value;
                    if (upper != lane.end()) value += (upper->value - lower.value) * static_cast<double>(sample - lower.sample) /
                        static_cast<double>(upper->sample - lower.sample);
                }
                blockParameters.push_back({frame, id, value});
            }
        }
        if (!instrument.process(left.data(), right.data(), block, blockEvents, error, nullptr, nullptr, blockParameters.empty() ? nullptr : &blockParameters)) {
            return fail(error.empty() ? "plugin process failed" : error);
        }
        if(instrument.currentLatency()!=latency) return fail("VST3 instrument changed latency during render");
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
