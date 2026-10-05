#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nodsynth/midi/Smf.h>

using namespace nodsynth::midi;

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
    std::uint16_t format{0};
    std::uint16_t ppq{480};
    std::vector<std::vector<std::uint8_t>> tracks{1};
    std::vector<std::uint32_t> endDeltas{0};

    void event(std::size_t track, std::uint32_t delta, std::initializer_list<std::uint8_t> bytes) {
        writeVarlen(tracks.at(track), delta);
        tracks.at(track).insert(tracks.at(track).end(), bytes);
    }

    void setEnd(std::size_t track, std::uint32_t delta) {
        if (endDeltas.size() <= track) endDeltas.resize(track + 1);
        endDeltas[track] = delta;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        std::vector<std::uint8_t> out;
        out.insert(out.end(), {'M', 'T', 'h', 'd'});
        write32(out, 6);
        write16(out, format);
        write16(out, static_cast<std::uint16_t>(tracks.size()));
        write16(out, ppq);
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

ParseResult parseBytes(const std::vector<std::uint8_t>& bytes, Limits limits = {}) {
    return parse(bytes.data(), bytes.size(), limits);
}

const Event* findKind(const File& file, EventKind kind) {
    for (const auto& event : file.events) {
        if (event.kind == kind) return &event;
    }
    return nullptr;
}
} // namespace

TEST_CASE("type 0 preserves running status, note-on velocity zero, and channel selection") {
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 100});
    builder.event(0, 480, {60, 0});
    builder.event(0, 0, {0x91, 64, 80});
    builder.event(0, 96, {64, 40});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.format == 0);
    REQUIRE(parsed.file.ppq == 480);
    REQUIRE(parsed.file.events.size() == 4);
    REQUIRE(parsed.file.events[0].kind == EventKind::noteOn);
    REQUIRE(parsed.file.events[0].channel == 0);
    REQUIRE(parsed.file.events[0].data1 == 60);
    REQUIRE(parsed.file.events[0].data2 == 100);
    REQUIRE(parsed.file.events[1].kind == EventKind::noteOff);
    REQUIRE(parsed.file.events[1].tick == 480);
    REQUIRE(parsed.file.events[1].data2 == 0);
    REQUIRE(parsed.file.events[2].channel == 1);
    REQUIRE(parsed.file.events[2].kind == EventKind::noteOn);
    REQUIRE(parsed.file.events[3].kind == EventKind::noteOn);
    REQUIRE(parsed.file.events[3].channel == 1);
    REQUIRE(parsed.file.events[3].tick == 576);
    REQUIRE(parsed.file.endTick == 576);

    REQUIRE(requiresExplicitStream(parsed.file));
    const auto streams = noteStreams(parsed.file);
    REQUIRE(streams.size() == 2);
    Selection channel;
    channel.channel = 1;
    const auto selected = select(parsed.file, channel);
    REQUIRE(selected.size() == 2);
    REQUIRE(selected[0].data1 == 64);
    REQUIRE(selected[1].data1 == 64);
}

TEST_CASE("end of track delta extends the file timeline") {
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 100});
    builder.setEnd(0, 960);
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.events[0].tick == 0);
    REQUIRE(parsed.file.endTick == 960);
}

TEST_CASE("type 1 orders events by tick, track, and source sequence") {
    SmfBuilder builder;
    builder.format = 1;
    builder.tracks.resize(2);
    builder.event(1, 0, {0x90, 60, 100});
    builder.event(0, 100, {0x90, 62, 90});
    builder.event(1, 100, {0x80, 60, 0});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.events.size() == 3);
    REQUIRE(parsed.file.events[0].track == 1);
    REQUIRE(parsed.file.events[0].tick == 0);
    REQUIRE(parsed.file.events[1].track == 0);
    REQUIRE(parsed.file.events[1].tick == 100);
    REQUIRE(parsed.file.events[2].track == 1);
    REQUIRE(parsed.file.events[2].tick == 100);
}

TEST_CASE("missing tempo and time signature fall back to 120 bpm and 4/4") {
    SmfBuilder builder;
    builder.event(0, 480, {0x90, 69, 100});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.tempo.size() == 1);
    REQUIRE(parsed.file.tempo[0].tick == 0);
    REQUIRE(parsed.file.tempo[0].microsecondsPerQuarter == kDefaultTempoUs);
    REQUIRE(parsed.file.timeSignatures.size() == 1);
    REQUIRE(parsed.file.timeSignatures[0].numerator == 4);
    REQUIRE(parsed.file.timeSignatures[0].denominator == 4);
    TempoMap map(parsed.file);
    REQUIRE(map.sampleAtTick(480, 48000) == 24000);
    REQUIRE(map.sampleAtTick(480, 44100) == 22050);
}

TEST_CASE("tempo changes and non-480 PPQ convert from absolute musical time") {
    SmfBuilder changed;
    changed.event(0, 0, {0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
    changed.event(0, 480, {0xff, 0x51, 0x03, 0x0f, 0x42, 0x40});
    changed.event(0, 480, {0x90, 60, 100});
    const auto parsed = parseBytes(changed.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.tempo.size() == 2);
    REQUIRE(parsed.file.tempo[1].tick == 480);
    REQUIRE(parsed.file.tempo[1].microsecondsPerQuarter == 1000000);
    TempoMap map(parsed.file);
    REQUIRE(map.sampleAtTick(480, 48000) == 24000);
    REQUIRE(map.sampleAtTick(960, 48000) == 72000);
    REQUIRE(map.sampleAtTick(960, 44100) == 66150);

    SmfBuilder coarse;
    coarse.ppq = 96;
    coarse.event(0, 0, {0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
    coarse.event(0, 96, {0x90, 67, 110});
    const auto coarseParsed = parseBytes(coarse.build());
    REQUIRE(coarseParsed.status == ParseStatus::ok);
    REQUIRE(coarseParsed.file.ppq == 96);
    TempoMap coarseMap(coarseParsed.file);
    REQUIRE(coarseMap.sampleAtTick(96, 48000) == 24000);
    REQUIRE(coarseMap.sampleAtTick(96, 44100) == 22050);

    SmfBuilder metered;
    metered.event(0, 0, {0xff, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08});
    metered.event(0, 480, {0x90, 60, 100});
    const auto meteredParsed = parseBytes(metered.build());
    REQUIRE(meteredParsed.file.timeSignatures[0].numerator == 3);
    REQUIRE(meteredParsed.file.timeSignatures[0].denominator == 4);
    REQUIRE(TempoMap(meteredParsed.file).sampleAtTick(480, 48000) == 24000);
}

TEST_CASE("conflicting tempo keeps the lowest track and diagnoses the rest") {
    SmfBuilder builder;
    builder.format = 1;
    builder.tracks.resize(2);
    builder.event(0, 0, {0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
    builder.event(0, 0, {0xff, 0x51, 0x03, 0x06, 0x1a, 0x80});
    builder.event(1, 0, {0xff, 0x51, 0x03, 0x0f, 0x42, 0x40});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.tempo.size() == 1);
    REQUIRE(parsed.file.tempo[0].microsecondsPerQuarter == 400000);
    REQUIRE(parsed.file.diagnostics.size() == 2);
    REQUIRE(parsed.file.diagnostics[0].code == "tempo-conflict");
    REQUIRE(parsed.file.diagnostics[1].code == "tempo-conflict");
    REQUIRE(parsed.file.diagnostics[1].track == 1);
}

TEST_CASE("playback puts a same-note retrigger off before on without rewriting source order") {
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 60, 100});
    builder.event(0, 0, {0x80, 60, 40});
    builder.event(0, 0, {0x90, 60, 90});
    builder.event(0, 0, {0xb0, 64, 127});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.file.events[0].kind == EventKind::noteOn);
    REQUIRE(parsed.file.events[1].kind == EventKind::noteOff);
    REQUIRE(parsed.file.events[2].kind == EventKind::noteOn);
    const auto scheduled = schedule(parsed.file, 48000, 128);
    REQUIRE(scheduled.status == ParseStatus::ok);
    REQUIRE(scheduled.events[0].source.kind == EventKind::noteOff);
    REQUIRE(scheduled.events[1].source.kind == EventKind::noteOn);
    REQUIRE(scheduled.events[1].source.data2 == 100);
    REQUIRE(scheduled.events[2].source.kind == EventKind::noteOn);
    REQUIRE(scheduled.events[2].source.data2 == 90);
    REQUIRE(scheduled.events[3].source.kind == EventKind::controlChange);
    REQUIRE(scheduled.events[3].source.data1 == 64);
}

TEST_CASE("sustain, pitch bend, and unsupported channel events stay addressable") {
    SmfBuilder builder;
    builder.event(0, 0, {0xb0, 64, 127});
    builder.event(0, 10, {0xe0, 0x00, 0x40});
    builder.event(0, 0, {0xb0, 7, 100});
    builder.event(0, 0, {0xc0, 12});
    builder.event(0, 0, {0xff, 0x03, 0x04, 'l', 'e', 'a', 'd'});
    builder.event(0, 0, {0xf0, 0x03, 0x01, 0x02, 0xf7});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(findKind(parsed.file, EventKind::controlChange)->data1 == 64);
    REQUIRE(findKind(parsed.file, EventKind::pitchBend)->data2 == 0x40);
    REQUIRE(findKind(parsed.file, EventKind::programChange)->data1 == 12);
    REQUIRE(findKind(parsed.file, EventKind::sysex)->payload.size() == 3);
    REQUIRE(findKind(parsed.file, EventKind::meta)->metaType == 0x03);

    const auto strict = assess(parsed.file, RenderMode::strict);
    REQUIRE(strict.rejected);
    REQUIRE(strict.unsupported.size() == 3);
    REQUIRE(strict.preserved.size() == 1);
    const auto loose = assess(parsed.file, RenderMode::loose);
    REQUIRE_FALSE(loose.rejected);
    REQUIRE(loose.unsupported.size() == 3);

    SynthCapabilities withoutBend = {};
    withoutBend.pitchBend = false;
    SmfBuilder bendOnly;
    bendOnly.event(0, 0, {0xe0, 0x00, 0x40});
    const auto bend = parseBytes(bendOnly.build());
    REQUIRE_FALSE(assess(bend.file, RenderMode::strict).rejected);
    REQUIRE(assess(bend.file, RenderMode::strict, withoutBend).rejected);
}

TEST_CASE("absolute samples are independent of block size and land inside the block") {
    SmfBuilder builder;
    builder.event(0, 3, {0x90, 60, 100});
    builder.event(0, 477, {0x80, 60, 0});
    const auto parsed = parseBytes(builder.build());
    bool seen = false;
    std::int64_t first = 0;
    std::int64_t second = 0;
    for (const auto block : {std::uint32_t{64}, std::uint32_t{128}, std::uint32_t{256}, std::uint32_t{512}}) {
        for (const auto rate : {std::uint32_t{44100}, std::uint32_t{48000}}) {
            const auto scheduled = schedule(parsed.file, rate, block);
            REQUIRE(scheduled.status == ParseStatus::ok);
            REQUIRE(scheduled.events.size() == 2);
            for (const auto& event : scheduled.events) {
                const auto rebuilt = static_cast<std::int64_t>(event.blockIndex * block + event.sampleOffset);
                REQUIRE(rebuilt == event.absoluteSample);
                REQUIRE(event.sampleOffset < block);
            }
            if (rate == 48000) {
                if (!seen) {
                    first = scheduled.events[0].absoluteSample;
                    second = scheduled.events[1].absoluteSample;
                    seen = true;
                } else {
                    REQUIRE(scheduled.events[0].absoluteSample == first);
                    REQUIRE(scheduled.events[1].absoluteSample == second);
                }
                if (block == 128) {
                    REQUIRE(scheduled.events[0].blockIndex == 1);
                    REQUIRE(scheduled.events[0].sampleOffset == 22);
                }
            }
            const auto ideal = static_cast<std::int64_t>(std::llround(3.0 * 500000.0 * rate / (480.0 * 1000000.0)));
            REQUIRE(std::llabs(scheduled.events[0].absoluteSample - ideal) <= 1);
        }
    }
}

TEST_CASE("a long tick delta still rounds from one absolute division") {
    SmfBuilder builder;
    const auto delta = 0x0fffffffu;
    builder.event(0, delta, {0x90, 60, 1});
    builder.event(0, delta, {0x80, 60, 0});
    builder.event(0, delta, {0x90, 62, 1});
    const auto parsed = parseBytes(builder.build());
    REQUIRE(parsed.status == ParseStatus::ok);
    const auto tick = delta * 3;
    const auto sample = TempoMap(parsed.file).sampleAtTick(tick, 48000);
    REQUIRE(sample.has_value());
    REQUIRE(*sample == static_cast<std::int64_t>(tick) * 50);
}

TEST_CASE("corrupt and hostile MIDI files are rejected") {
    REQUIRE(parse(nullptr, 0).code == "truncated");

    SmfBuilder type2;
    type2.format = 2;
    REQUIRE(parseBytes(type2.build()).code == "unsupported-format");

    auto smpte = SmfBuilder{}.build();
    smpte[12] = 0xe2;
    smpte[13] = 0x50;
    REQUIRE(parseBytes(smpte).code == "smpte-division");

    SmfBuilder truncated;
    truncated.event(0, 0, {0x90, 60, 100});
    auto bytes = truncated.build();
    bytes.resize(bytes.size() / 2);
    REQUIRE(parseBytes(bytes).code == "truncated");

    std::vector<std::uint8_t> missingEnd{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, 4, 0x00, 0x90, 60, 100};
    REQUIRE(parseBytes(missingEnd).code == "missing-end-of-track");

    std::vector<std::uint8_t> longVarlen{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, 8,
                                         0x80, 0x80, 0x80, 0x80, 0x00, 0xff, 0x2f, 0x00};
    REQUIRE(parseBytes(longVarlen).code == "bad-varlen");

    SmfBuilder limited;
    limited.event(0, 0, {0x90, 60, 10});
    limited.event(0, 1, {0x80, 60, 0});
    Limits limits;
    limits.maxEvents = 1;
    REQUIRE(parseBytes(limited.build(), limits).code == "too-many-events");
    limits = {};
    limits.maxFileBytes = 8;
    REQUIRE(parseBytes(limited.build(), limits).code == "file-too-large");
    limits = {};
    limits.maxVarlenBytes = 1;
    SmfBuilder wideDelta;
    wideDelta.event(0, 128, {0x90, 60, 10});
    REQUIRE(parseBytes(wideDelta.build(), limits).code == "bad-varlen");

    auto trailing = SmfBuilder{}.build();
    trailing.push_back(0);
    REQUIRE(parseBytes(trailing).code == "trailing-data");

    SmfBuilder type0;
    type0.format = 0;
    type0.tracks.resize(2);
    REQUIRE(parseBytes(type0.build()).code == "type0-track-count");

    SmfBuilder running;
    running.event(0, 0, {0x90, 60, 100});
    running.event(0, 0, {0xff, 0x01, 0x01, 'a'});
    running.event(0, 0, {60, 0});
    REQUIRE(parseBytes(running.build()).code == "running-status-missing");
}

TEST_CASE("parseFile reads a MIDI file from disk") {
    SmfBuilder builder;
    builder.event(0, 0, {0x90, 69, 100});
    const auto bytes = builder.build();
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-smoke.mid";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto parsed = parseFile(path);
    std::filesystem::remove(path);
    REQUIRE(parsed.status == ParseStatus::ok);
    REQUIRE(parsed.file.events.size() == 1);
    REQUIRE(parsed.file.events[0].data1 == 69);
}
