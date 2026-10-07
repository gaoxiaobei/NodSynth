#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/nodes/Unison.h>
#include <nodsynth/runtime/Engine.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

using namespace nodsynth;

namespace {
struct Source {
    std::uint32_t capacity;
    std::vector<float> frequency, leftRight, parameters, triggers, velocity, notes;
    std::array<const float*, 2> triggerPointers{}, velocityPointers{}, notePointers{};
    std::unique_ptr<runtime::DspNode> node{nodes::makeUnisonOscillator()};

    Source(double rate, std::uint32_t frames, std::uint32_t voices = 1)
        : capacity(frames), frequency(frames * voices, 440), leftRight(frames * 2 * voices),
          parameters(frames * 8), triggers(frames * voices), velocity(frames * voices, 1), notes(frames * voices, 69) {
        const std::array<float, 8> defaults{7, 18, .8f, 1, .6f, .2f, 123, 440};
        for (std::uint32_t index = 0; index < 8; ++index) set(index, defaults[index]);
        for (std::uint32_t voice = 0; voice < voices; ++voice) {
            triggers[voice * frames] = 1;
            triggerPointers[voice] = triggers.data() + voice * frames;
            velocityPointers[voice] = velocity.data() + voice * frames;
            notePointers[voice] = notes.data() + voice * frames;
        }
        runtime::NodeBinding binding;
        binding.sampleRate = rate; binding.maxFrames = frames; binding.voiceCount = voices;
        binding.inputs = {{{"frequency"}, frequency.data(), 1, frames, frames, frames}};
        binding.outputs = {{{"audio"}, leftRight.data(), 2, frames, frames * 2, frames}};
        binding.parameters = {parameters.data(), 8, frames};
        binding.triggers = triggerPointers.data(); binding.velocity = velocityPointers.data(); binding.note = notePointers.data();
        node->bind(binding);
    }
    void set(std::uint32_t index, float value) { std::fill_n(parameters.data() + index * capacity, capacity, value); }
    void process(std::uint32_t voice = 0) { node->process(voice, capacity); }
};

std::vector<std::complex<double>> fft(const std::vector<float>& input) {
    const auto count = input.size();
    std::vector<std::complex<double>> bins(input.begin(), input.end());
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        auto bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(bins[i], bins[j]);
    }
    for (std::size_t length = 2; length <= count; length *= 2) {
        const auto step = std::polar(1.0, -2 * std::numbers::pi / length);
        for (std::size_t start = 0; start < count; start += length) {
            std::complex<double> phase{1, 0};
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto a = bins[start + offset], b = bins[start + offset + length / 2] * phase;
                bins[start + offset] = a + b; bins[start + offset + length / 2] = a - b;
                phase *= step;
            }
        }
    }
    return bins;
}

double unwantedEnergy(const std::vector<float>& input, std::uint32_t fundamental) {
    const auto spectrum = fft(input);
    double total = 0, unwanted = 0;
    for (std::size_t bin = 0; bin <= input.size() / 2; ++bin) {
        const auto energy = std::norm(spectrum[bin]);
        total += energy;
        if (bin == 0 || bin % fundamental != 0) unwanted += energy;
    }
    return unwanted / total;
}

std::vector<float> chord(std::uint32_t rate, std::uint32_t block) {
    runtime::EngineConfig config; config.audio.sampleRate = rate; config.audio.maxFrames = block;
    config.parameterSmoothSeconds = 0;
    runtime::Engine engine(config);
    auto graph = nodes::unisonPatch();
    graph.nodes[2].parameters[model::ParameterId{"voices"}] = 8;
    const auto registry = nodes::builtinRegistry();
    const auto compiled = compiler::GraphCompiler{}.compile(graph, registry); REQUIRE(compiled.graph);
    auto prepared = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), engine.config(), engine.voices(), 0);
    REQUIRE(prepared.plan); REQUIRE(prepared.plan->committedBytes() >= nodes::unisonStateBytes(16));
    REQUIRE(engine.stage(std::move(prepared.plan)).accepted);
    std::array<runtime::MidiEvent, 16> events{};
    for (std::uint8_t index = 0; index < 16; ++index) events[index] = {0, runtime::MidiType::noteOn, 0, static_cast<std::uint8_t>(48 + index), 100};
    const auto frames = rate / 5;
    std::vector<float> result(frames * 2), left(block), right(block);
    for (std::uint32_t start = 0; start < frames; start += block) {
        const auto count = std::min(block, frames - start);
        float* outputs[]{left.data(), right.data()};
        engine.process(outputs, 2, count, start ? std::span<const runtime::MidiEvent>{} : std::span<const runtime::MidiEvent>(events));
        for (std::uint32_t frame = 0; frame < count; ++frame) {
            result[(start + frame) * 2] = left[frame]; result[(start + frame) * 2 + 1] = right[frame];
        }
    }
    return result;
}
}

TEST_CASE("Q2 Unison harmonic banks suppress alias images at low and high pitches", "[production][unison]") {
    constexpr std::uint32_t frames = 8192;
    for (const auto rate : {44100u, 48000u, 96000u}) for (const auto bin : {47u, 997u, 1487u}) {
        Source source(rate, frames);
        source.set(0, 1); source.set(1, 0); source.set(2, 0); source.set(3, 0); source.set(5, 1);
        std::fill(source.frequency.begin(), source.frequency.end(), static_cast<float>(static_cast<double>(rate) * bin / frames));
        source.process();
        const std::vector<float> output(source.leftRight.begin(), source.leftRight.begin() + frames);
        const double alias = unwantedEnergy(output, bin);
        INFO("rate=" << rate << " bin=" << bin << " alias-energy=" << alias);
        REQUIRE(alias < 1e-9);
        const auto spectrum = fft(output);
        const auto fundamental = 2 * std::abs(spectrum[bin]) / frames;
        REQUIRE(fundamental == Catch::Approx(2 / std::numbers::pi).margin(.00001));
    }
}

TEST_CASE("Q2 Unison level normalization seed and phase are explicit", "[production][unison]") {
    Source mono(48000, 4096), coherent(48000, 4096), random(48000, 4096), repeat(48000, 4096);
    mono.set(0, 1); mono.set(1, 0); mono.set(3, 0);
    coherent.set(0, 8); coherent.set(1, 0); coherent.set(3, 0);
    mono.process(); coherent.process(); random.process(); repeat.process();
    REQUIRE(random.leftRight == repeat.leftRight);
    REQUIRE(random.leftRight != mono.leftRight);
    for (std::uint32_t sample = 0; sample < 4096; ++sample) {
        REQUIRE(coherent.leftRight[sample] == Catch::Approx(mono.leftRight[sample]).margin(1e-7));
        REQUIRE(coherent.leftRight[sample] == coherent.leftRight[4096 + sample]);
    }
    double width = 0;
    for (std::uint32_t sample = 0; sample < 4096; ++sample) width += std::fabs(random.leftRight[sample] - random.leftRight[4096 + sample]);
    REQUIRE(width > 1);
    repeat.set(6, 124); repeat.node->reset(); repeat.process(); REQUIRE(random.leftRight != repeat.leftRight);
    for (std::uint32_t sample = 0; sample < 4096; ++sample) random.frequency[sample] = sample % 2 ? 19000.f : 30.f;
    random.process();
    for (const auto value : random.leftRight) REQUIRE(std::isfinite(value));
}

TEST_CASE("Q2 Unison random phases are independent of voice processing order", "[production][unison]") {
    Source first(48000, 128, 2), second(48000, 128, 2);
    first.process(0); first.process(1); second.process(1); second.process(0);
    REQUIRE(first.leftRight == second.leftRight);
}

TEST_CASE("Q2 sixteen notes times eight oscillators repeat across rates and blocks", "[production][unison]") {
    for (const auto rate : {44100u, 48000u, 96000u}) {
        const auto reference = chord(rate, 128);
        for (const auto block : {64u, 512u}) {
            const auto other = chord(rate, block);
            REQUIRE(reference == other);
        }
        for (const auto value : reference) REQUIRE(std::isfinite(value));
    }
}

TEST_CASE("Q2 long maximum Unison chords survive sustain stealing and note termination", "[production][unison][long-polyphony]") {
    for(auto rate:{44100u,48000u,96000u}) {
        const auto render=[&](std::uint32_t block) {
            runtime::EngineConfig config;config.audio.sampleRate=rate;config.audio.maxFrames=block;config.parameterSmoothSeconds=0;
            runtime::Engine engine(config);auto graph=nodes::unisonPatch();graph.nodes[2].parameters[model::ParameterId{"voices"}]=8;
            const auto registry=nodes::builtinRegistry();const auto compiled=compiler::GraphCompiler{}.compile(graph,registry);REQUIRE(compiled.graph);
            auto prepared=runtime::preparePlan(*compiled.graph,registry,nodes::builtinImplementations(),engine.config(),engine.voices(),0);
            REQUIRE(prepared.plan);REQUIRE(engine.stage(std::move(prepared.plan)).accepted);
            std::vector<std::pair<std::uint32_t,runtime::MidiEvent>> timeline{{0,{0,runtime::MidiType::sustain,0,64,127}}};
            for(std::uint8_t note=48;note<64;++note) timeline.push_back({0,{0,runtime::MidiType::noteOn,0,note,127}});
            for(std::uint8_t note=72;note<80;++note) timeline.push_back({rate,{0,runtime::MidiType::noteOn,0,note,120}});
            timeline.push_back({rate*2,{0,runtime::MidiType::allNotesOff,0,123,0}});
            timeline.push_back({rate*3,{0,runtime::MidiType::sustain,0,64,0}});
            timeline.push_back({rate*4,{0,runtime::MidiType::allSoundOff,0,120,0}});
            std::vector<float> left(block),right(block);std::vector<runtime::MidiEvent> events;events.reserve(32);
            std::uint64_t hash=14695981039346656037ull;std::size_t cursor=0;double tailEnergy=0,peak=0;
            for(std::uint32_t begin=0;begin<rate*6;begin+=block) {
                const auto frames=std::min(block,rate*6-begin);events.clear();
                while(cursor<timeline.size() && timeline[cursor].first<begin+frames) {
                    auto event=timeline[cursor].second;event.sampleOffset=timeline[cursor].first-begin;events.push_back(event);++cursor;
                }
                float* outputs[]{left.data(),right.data()};engine.process(outputs,2,frames,events);
                for(std::uint32_t frame=0;frame<frames;++frame) for(float sample:{left[frame],right[frame]}) {
                    REQUIRE(std::isfinite(sample));peak=std::max(peak,std::fabs(static_cast<double>(sample)));
                    if(begin+frame>=rate*5) tailEnergy+=sample*sample;
                    const auto* bytes=reinterpret_cast<const unsigned char*>(&sample);for(std::size_t byte=0;byte<sizeof(sample);++byte) {hash^=bytes[byte];hash*=1099511628211ull;}
                }
            }
            REQUIRE(engine.voices().activeCount()==0);REQUIRE(tailEnergy<1e-10);REQUIRE(peak>0);REQUIRE(peak<4);return hash;
        };
        const auto reference=render(128);REQUIRE(render(64)==reference);REQUIRE(render(512)==reference);
    }
}
