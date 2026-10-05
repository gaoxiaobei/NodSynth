#include <nodsynth/song/SongRenderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/song/Process.h>

namespace nodsynth::song {
namespace {
SongRenderReport fail(std::string code, std::string message, std::vector<Diagnostic> diagnostics = {}) {
    SongRenderReport report;
    report.code = std::move(code);
    report.message = std::move(message);
    report.diagnostics = std::move(diagnostics);
    return report;
}

std::filesystem::path resolvePath(const std::filesystem::path& base, const std::string& stored) {
    const std::filesystem::path path(stored);
    if (path.is_absolute() || base.empty()) return path;
    return base / path;
}

bool safeTrackId(const std::string& id) {
    return !id.empty() && id.find('/') == std::string::npos && id.find('\\') == std::string::npos &&
           id.find("..") == std::string::npos;
}

void equalPower(double gain, double pan, float& left, float& right) {
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * 0.78539816339744830962;
    left = static_cast<float>(gain * std::cos(angle));
    right = static_cast<float>(gain * std::sin(angle));
}

struct TimedEvent {
    std::int64_t sample{0};
    int priority{0};
    std::uint32_t order{0};
    runtime::MidiEvent midi{};
};

struct GainSample {
    std::int64_t sample{0};
    float gain{1.f};
};

float gainAt(const std::vector<GainSample>& points, std::int64_t sample, float constant) {
    if (points.empty()) return constant;
    if (sample <= points.front().sample) return points.front().gain;
    if (sample >= points.back().sample) return points.back().gain;
    auto upper = std::upper_bound(points.begin(), points.end(), sample, [](std::int64_t value, const GainSample& point) {
        return value < point.sample;
    });
    if (upper == points.begin()) return upper->gain;
    const auto lower = upper - 1;
    if (upper == points.end() || upper->sample == lower->sample) return lower->gain;
    const double span = static_cast<double>(upper->sample - lower->sample);
    const double position = static_cast<double>(sample - lower->sample) / span;
    return static_cast<float>(lower->gain + position * (upper->gain - lower->gain));
}

bool toEngine(const PerformanceEvent& event, runtime::MidiEvent& midi) {
    midi.channel = event.channel;
    midi.data1 = event.data1;
    midi.data2 = event.data2;
    switch (event.kind) {
    case midi::EventKind::pitchBend:
        midi.type = runtime::MidiType::pitchBend;
        return true;
    case midi::EventKind::controlChange:
        if (event.data1 == 64) midi.type = runtime::MidiType::sustain;
        else if (event.data1 == 120) midi.type = runtime::MidiType::allSoundOff;
        else if (event.data1 == 123) midi.type = runtime::MidiType::allNotesOff;
        else return false;
        return true;
    default:
        return false;
    }
}

std::uint32_t readLe32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t readLe16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct DecodedWav {
    std::uint32_t sampleRate{0};
    std::uint32_t channels{0};
    std::vector<float> interleaved;
};

bool decodeWav(const std::filesystem::path& path, std::uint32_t expectedRate, DecodedWav& wav, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open the external audio file";
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::string(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<const char*>(bytes.data() + 8), 4) != "WAVE") {
        error = "external audio is not a WAV file";
        return false;
    }
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bits = 0;
    const unsigned char* data = nullptr;
    std::uint32_t dataBytes = 0;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto chunk = std::string(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        const auto size = readLe32(bytes.data() + offset + 4);
        if (offset + 8 + size > bytes.size()) {
            error = "external WAV chunk is truncated";
            return false;
        }
        const auto* body = bytes.data() + offset + 8;
        if (chunk == "fmt ") {
            if (size < 16) {
                error = "external WAV format is truncated";
                return false;
            }
            format = readLe16(body);
            channels = readLe16(body + 2);
            sampleRate = readLe32(body + 4);
            bits = readLe16(body + 14);
        } else if (chunk == "data") {
            data = body;
            dataBytes = size;
        }
        offset += 8ull + size + (size & 1u);
    }
    if (data == nullptr || channels == 0 || sampleRate == 0) {
        error = "external WAV is missing audio data";
        return false;
    }
    if (sampleRate != expectedRate) {
        error = "external audio sample rate does not match the render";
        return false;
    }
    if (channels > 2 || (format == 3 && bits != 32) || (format == 1 && bits != 16) || (format != 1 && format != 3)) {
        error = "external audio must be mono or stereo 16-bit PCM or float32";
        return false;
    }
    const auto frameBytes = static_cast<std::uint32_t>(channels * (bits / 8));
    if (frameBytes == 0 || dataBytes % frameBytes != 0) {
        error = "external audio frame size is invalid";
        return false;
    }
    const auto frames = dataBytes / frameBytes;
    wav.sampleRate = sampleRate;
    wav.channels = 2;
    wav.interleaved.assign(static_cast<std::size_t>(frames) * 2, 0.f);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            const auto* sample = data + (static_cast<std::size_t>(frame) * channels + channel) * (bits / 8);
            float value = 0.f;
            if (format == 3) std::memcpy(&value, sample, sizeof(float));
            else value = static_cast<float>(static_cast<std::int16_t>(readLe16(sample))) / 32768.f;
            wav.interleaved[static_cast<std::size_t>(frame) * 2 + channel] = value;
            if (channels == 1) wav.interleaved[static_cast<std::size_t>(frame) * 2 + 1] = value;
        }
    }
    return true;
}

struct TrackVoice {
    std::string trackId;
    std::string adapter{"nodsynth"};
    double pan{0};
    float gain{1.f};
    std::vector<GainSample> automation;
    std::unique_ptr<runtime::Engine> engine;
    std::vector<float> external;
    bool externalTrack{false};
    std::filesystem::path asset;
    std::string className;
    std::uint32_t latencySamples{0};
    struct ParameterSample {
        std::int64_t sample{0};
        std::string name;
        double value{0};
    };
    std::vector<ParameterSample> parameters;
    std::vector<TimedEvent> events;
    std::size_t cursor{0};
    std::size_t externalCursor{0};
    float peak{0.f};
    std::uint64_t hash{14695981039346656037ull};
    std::unique_ptr<runtime::WavStream> stem;
    std::filesystem::path stemPath;
    std::unique_ptr<runtime::WavStream> dry;
    std::filesystem::path dryFile;
    std::string soundKey;
};

std::uint64_t mixHash(std::uint64_t hash, const float* samples, std::size_t count) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    const auto size = count * sizeof(float);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] = digits[value & 0xf];
        value >>= 4;
    }
    return text;
}

std::string hashText(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hex64(hash);
}

persist::Json sharedRenderJson(const SongDocument& song, const SongRenderOptions& options) {
    persist::Json json = persist::Json::object();
    json.set("ppq", persist::Json::number(song.ppq));
    json.set("sampleRate", persist::Json::number(options.sampleRate));
    json.set("blockSize", persist::Json::number(options.blockSize));
    json.set("tailSeconds", persist::Json::number(options.tailSeconds));
    json.set("quality", persist::Json::string(options.quality.empty() ? "final" : options.quality));
    json.set("endTick", persist::Json::number(endTick(song)));
    persist::Json tempo = persist::Json::array();
    for (const auto& point : song.tempo) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(point.tick));
        item.set("us", persist::Json::number(point.microsecondsPerQuarter));
        tempo.push(std::move(item));
    }
    json.set("tempo", std::move(tempo));
    persist::Json signatures = persist::Json::array();
    for (const auto& point : song.timeSignatures) {
        persist::Json item = persist::Json::object();
        item.set("tick", persist::Json::number(point.tick));
        item.set("n", persist::Json::number(point.numerator));
        item.set("d", persist::Json::number(point.denominator));
        signatures.push(std::move(item));
    }
    json.set("timeSignatures", std::move(signatures));
    return json;
}

std::string trackSoundKey(const SongDocument& song, const Track& track, const SongRenderOptions& options) {
    persist::Json json = sharedRenderJson(song, options);
    json.set("track", persist::Json::string(track.id));
    json.set("instrument", persist::Json::string(track.instrumentId));
    persist::Json notes = persist::Json::array();
    for (const auto& clip : track.clips) {
        for (const auto& note : clip.notes) {
            persist::Json noteJson = persist::Json::object();
            noteJson.set("t", persist::Json::number(static_cast<double>(clip.startTick) + note.tick));
            noteJson.set("d", persist::Json::number(note.duration));
            noteJson.set("p", persist::Json::number(note.pitch));
            noteJson.set("v", persist::Json::number(note.velocity));
            noteJson.set("c", persist::Json::number(note.channel));
            notes.push(std::move(noteJson));
        }
    }
    json.set("notes", std::move(notes));
    persist::Json performance = persist::Json::array();
    for (const auto& event : track.performance) {
        persist::Json item = persist::Json::object();
        item.set("t", persist::Json::number(event.tick));
        item.set("k", persist::Json::number(static_cast<double>(event.kind)));
        item.set("d1", persist::Json::number(event.data1));
        item.set("d2", persist::Json::number(event.data2));
        performance.push(std::move(item));
    }
    json.set("performance", std::move(performance));
    persist::Json parameters = persist::Json::array();
    for (const auto& lane : track.parameterAutomation) {
        persist::Json laneJson = persist::Json::object();
        laneJson.set("id", persist::Json::string(lane.id));
        persist::Json points = persist::Json::array();
        for (const auto& point : lane.points) {
            persist::Json pointJson = persist::Json::object();
            pointJson.set("t", persist::Json::number(point.tick));
            pointJson.set("v", persist::Json::number(point.value));
            points.push(std::move(pointJson));
        }
        laneJson.set("points", std::move(points));
        parameters.push(std::move(laneJson));
    }
    json.set("parameterAutomation", std::move(parameters));
    for (const auto& instrument : song.instruments) {
        if (instrument.id != track.instrumentId) continue;
        json.set("kind", persist::Json::string(instrument.kind == InstrumentKind::nodsynth ? "nodsynth" : instrument.adapter));
        for (const auto& resource : song.resources) {
            if (resource.id != instrument.resourceId) continue;
            json.set("resourceHash", persist::Json::string(resource.hash));
            json.set("resourcePath", persist::Json::string(resource.path));
        }
    }
    return hashText(json.dump(-1));
}

std::string fingerprint(const SongDocument& song, const SongRenderOptions& options, bool includeMix) {
    persist::Json json = sharedRenderJson(song, options);
    persist::Json tracks = persist::Json::array();
    for (const auto& track : song.tracks) {
        persist::Json item = persist::Json::object();
        item.set("id", persist::Json::string(track.id));
        item.set("sound", persist::Json::string(trackSoundKey(song, track, options)));
        if (includeMix) {
            item.set("gain", persist::Json::number(track.gain));
            item.set("pan", persist::Json::number(track.pan));
            persist::Json automation = persist::Json::array();
            for (const auto& point : track.gainAutomation) {
                persist::Json gain = persist::Json::object();
                gain.set("tick", persist::Json::number(point.tick));
                gain.set("gain", persist::Json::number(point.gain));
                automation.push(std::move(gain));
            }
            item.set("gainAutomation", std::move(automation));
        }
        tracks.push(std::move(item));
    }
    json.set("tracks", std::move(tracks));
    return hashText(json.dump(-1));
}

std::filesystem::path dryPath(const std::filesystem::path& cacheDir, const std::string& key) {
    return cacheDir / "dry" / (key + ".wav");
}

bool trackHasMusic(const Track& track) {
    if (!track.performance.empty()) return true;
    return std::any_of(track.clips.begin(), track.clips.end(), [](const Clip& clip) { return !clip.notes.empty(); });
}

const Instrument* findInstrument(const SongDocument& song, const std::string& id) {
    for (const auto& instrument : song.instruments) {
        if (instrument.id == id) return &instrument;
    }
    return nullptr;
}
} // namespace

persist::Json songReportJson(const SongRenderReport& report) {
    persist::Json json = persist::Json::object();
    json.set("status", persist::Json::string(report.ok ? "ok" : "rejected"));
    json.set("code", persist::Json::string(report.code));
    json.set("message", persist::Json::string(report.message));
    json.set("revision", persist::Json::number(static_cast<double>(report.revision)));
    json.set("sampleRate", persist::Json::number(report.sampleRate));
    json.set("blockSize", persist::Json::number(report.blockSize));
    json.set("frames", persist::Json::number(static_cast<double>(report.frames)));
    json.set("originSample", persist::Json::number(static_cast<double>(report.originSample)));
    json.set("tailFrames", persist::Json::number(static_cast<double>(report.tailFrames)));
    json.set("tailTruncated", persist::Json::boolean(report.tailTruncated));
    json.set("peak", persist::Json::number(report.peak));
    json.set("mixHash", persist::Json::string(report.mixHash));
    json.set("songHash", persist::Json::string(report.songHash));
    json.set("milliseconds", persist::Json::number(report.milliseconds));
    persist::Json timing = persist::Json::object();
    timing.set("prepareMs", persist::Json::number(report.timing.prepareMs));
    timing.set("prerollMs", persist::Json::number(report.timing.prerollMs));
    timing.set("dspMs", persist::Json::number(report.timing.dspMs));
    timing.set("externalMs", persist::Json::number(report.timing.externalMs));
    timing.set("mixMs", persist::Json::number(report.timing.mixMs));
    timing.set("writeMs", persist::Json::number(report.timing.writeMs));
    timing.set("analyzeMs", persist::Json::number(report.timing.analyzeMs));
    timing.set("totalMs", persist::Json::number(report.timing.totalMs));
    timing.set("renderedFrames", persist::Json::number(static_cast<double>(report.timing.renderedFrames)));
    timing.set("emittedFrames", persist::Json::number(static_cast<double>(report.timing.emittedFrames)));
    timing.set("realtimeFactor", persist::Json::number(report.timing.realtimeFactor));
    json.set("timing", std::move(timing));
    json.set("cacheHit", persist::Json::boolean(report.cacheHit));
    json.set("cacheReason", persist::Json::string(report.cacheReason));
    json.set("quality", persist::Json::string(report.quality));
    json.set("renderId", persist::Json::string(report.renderId));
    json.set("auditionStatus", persist::Json::string(report.auditionStatus));
    json.set("mixPath", persist::Json::string(report.mixPath));
    if (report.previewStartTick) json.set("previewStartTick", persist::Json::number(*report.previewStartTick));
    if (report.previewEndTick) json.set("previewEndTick", persist::Json::number(*report.previewEndTick));
    json.set("latencySamples", persist::Json::number(0));
    persist::Json stems = persist::Json::array();
    for (const auto& stem : report.stems) {
        persist::Json item = persist::Json::object();
        item.set("track", persist::Json::string(stem.trackId));
        item.set("path", persist::Json::string(stem.path));
        item.set("hash", persist::Json::string(stem.hash));
        item.set("peak", persist::Json::number(stem.peak));
        item.set("adapter", persist::Json::string(stem.adapter));
        item.set("latencySamples", persist::Json::number(stem.latencySamples));
        stems.push(std::move(item));
    }
    json.set("stems", std::move(stems));
    persist::Json diagnostics = persist::Json::array();
    for (const auto& diagnostic : report.diagnostics) {
        persist::Json item = persist::Json::object();
        item.set("code", persist::Json::string(diagnostic.code));
        item.set("message", persist::Json::string(diagnostic.message));
        item.set("tick", persist::Json::number(diagnostic.tick));
        item.set("track", persist::Json::number(diagnostic.track));
        diagnostics.push(std::move(item));
    }
    json.set("warnings", std::move(diagnostics));
    return json;
}

SongRenderReport renderSong(
    const SongDocument& song,
    const SongRenderOptions& options,
    const std::filesystem::path& mixOutput,
    const std::filesystem::path& stemsDirectory) {
    const auto started = std::chrono::steady_clock::now();
    const auto now = []() { return std::chrono::steady_clock::now(); };
    const auto msBetween = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    double prepareMs = 0;
    double prerollMs = 0;
    double dspMs = 0;
    double externalMs = 0;
    double mixMs = 0;
    double writeMs = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t emittedFrames = 0;
    auto externalStarted = started;
    bool externalRunning = false;
    auto finish = [&](SongRenderReport report) {
        report.milliseconds = msBetween(started, now());
        if (prepareMs == 0 && dspMs == 0 && externalMs == 0 && mixMs == 0 && writeMs == 0 && prerollMs == 0) {
            report.timing.prepareMs = report.milliseconds;
        } else {
            report.timing.prepareMs = prepareMs;
        }
        report.timing.prerollMs = prerollMs;
        report.timing.dspMs = dspMs;
        report.timing.externalMs = externalRunning ? msBetween(externalStarted, now()) : externalMs;
        report.timing.mixMs = mixMs;
        report.timing.writeMs = writeMs;
        report.timing.analyzeMs = 0;
        report.timing.totalMs = report.milliseconds;
        if (report.timing.renderedFrames == 0) report.timing.renderedFrames = renderedFrames;
        if (report.timing.emittedFrames == 0) report.timing.emittedFrames = emittedFrames;
        if (report.milliseconds > 0.0 && options.sampleRate > 0.0) {
            report.timing.realtimeFactor =
                (static_cast<double>(report.timing.emittedFrames) / options.sampleRate) / (report.milliseconds * 0.001);
        }
        if (report.cacheReason.empty()) report.cacheReason = "cache-not-implemented";
        if (report.quality.empty()) report.quality = "final";
        if (report.auditionStatus.empty()) report.auditionStatus = "unheard";
        if (report.quality.empty()) report.quality = options.quality.empty() ? "final" : options.quality;
        if (report.mixPath.empty()) report.mixPath = mixOutput.string();
        report.previewStartTick = options.previewStartTick;
        report.previewEndTick = options.previewEndTick;
        if (report.renderId.empty()) {
            report.renderId = hex64(static_cast<std::uint64_t>(now().time_since_epoch().count()));
            if (!report.mixHash.empty()) report.renderId += report.mixHash;
        }
        report.sampleRate = static_cast<std::uint32_t>(options.sampleRate);
        report.blockSize = options.blockSize;
        report.revision = song.revision;
        report.songHash = options.songHash;
        return report;
    };
    if (!runtime::validSampleRate(options.sampleRate) || options.sampleRate != std::floor(options.sampleRate) ||
        !runtime::validFrameCount(options.blockSize) || options.tailSeconds < 0.0 || options.maxTailSeconds < 0.0 ||
        options.tailThreshold < 0.0 || options.maxEventsPerBlock == 0 || mixOutput.empty()) {
        return finish(fail("invalid-options", "sample rate, block size, tail, or output is invalid"));
    }
    const auto validation = validate(song);
    if (!validation.ok) return finish(fail("invalid-song", "song failed validation", validation.diagnostics));
    if (options.mode == midi::RenderMode::strict) {
        std::vector<Diagnostic> unsupported;
        for (const auto& event : song.preserved) {
            if (!event.affectsSound) continue;
            unsupported.push_back(
                {"unsupported-event", std::string(midi::eventKindName(event.kind)) + " is not rendered", event.tick, event.sourceTrack});
        }
        if (!unsupported.empty()) return finish(fail("unsupported-event", "strict render rejected unsupported events", unsupported));
    }

    const auto rate = static_cast<std::uint32_t>(options.sampleRate);
    const auto mixKey = fingerprint(song, options, true);
    const auto cacheDir = options.cacheDirectory.empty() ? mixOutput.parent_path() / ".nod-cache" : options.cacheDirectory;
    const auto cachedMix = cacheDir / mixKey / "mix.wav";
    const bool preview = options.previewStartTick.has_value() || options.previewEndTick.has_value();
    if (options.useCache && preview && std::filesystem::exists(cachedMix)) {
        if (!options.previewStartTick || !options.previewEndTick || *options.previewEndTick < *options.previewStartTick) {
            return finish(fail("invalid-options", "preview requires a start tick and an end tick"));
        }
        const auto startSample = sampleAtTick(song, *options.previewStartTick, rate);
        const auto endSampleTick = sampleAtTick(song, *options.previewEndTick, rate);
        if (!startSample || !endSampleTick) return finish(fail("sample-overflow", "preview time does not fit in a sample index"));
        const auto tailBudget = static_cast<std::uint64_t>(std::llround(options.tailSeconds * options.sampleRate));
        const auto emitFrom = static_cast<std::uint64_t>(*startSample);
        const auto wantedUntil = static_cast<std::uint64_t>(*endSampleTick) + tailBudget;
        if (wantedUntil < emitFrom) return finish(fail("invalid-options", "preview range is empty"));
        runtime::WavData sliced;
        std::string wavError;
        if (!runtime::readWavRange(cachedMix, sliced, emitFrom, wantedUntil - emitFrom, wavError) || sliced.channels != 2 ||
            sliced.sampleRate != rate) {
            return finish(fail("cache-invalid", wavError.empty() ? "cached mix is unusable" : wavError));
        }
        if (!runtime::writeWav(mixOutput, sliced, wavError)) return finish(fail("output-io", wavError));
        SongRenderReport report;
        report.ok = true;
        report.frames = sliced.interleaved.size() / 2;
        report.originSample = emitFrom;
        report.tailFrames = tailBudget;
        report.message = "sliced";
        report.cacheHit = true;
        report.cacheReason = "slice-finished-mix";
        float peak = 0.f;
        for (float sample : sliced.interleaved) peak = std::max(peak, std::fabs(sample));
        report.peak = peak;
        report.mixHash = mixKey;
        return finish(std::move(report));
    }

    if (options.useCache && (options.quality.empty() || options.quality == "final")) {
        struct CachedTrack {
            const Track* track{nullptr};
            std::filesystem::path dry;
            bool external{false};
        };
        std::vector<CachedTrack> cached;
        bool remixable = true;
        for (const auto& track : song.tracks) {
            if (!trackHasMusic(track) && track.instrumentId.empty()) continue;
            const auto* instrument = findInstrument(song, track.instrumentId);
            if (instrument == nullptr) {
                remixable = false;
                break;
            }
            CachedTrack item;
            item.track = &track;
            if (instrument->kind == InstrumentKind::nodsynth) {
                item.dry = dryPath(cacheDir, trackSoundKey(song, track, options));
                if (!std::filesystem::exists(item.dry)) {
                    remixable = false;
                    break;
                }
            } else {
                item.external = true;
                if (!options.freezeExternal) {
                    remixable = false;
                    break;
                }
                item.dry = dryPath(cacheDir, trackSoundKey(song, track, options));
                if (!std::filesystem::exists(item.dry)) {
                    remixable = false;
                    break;
                }
            }
            cached.push_back(std::move(item));
        }
        if (remixable && !cached.empty()) {
            std::uint64_t emitFrom = 0;
            std::uint64_t emitUntil = ~std::uint64_t{0};
            const auto tailBudget = static_cast<std::uint64_t>(std::llround(options.tailSeconds * options.sampleRate));
            if (preview) {
                if (!options.previewStartTick || !options.previewEndTick || *options.previewEndTick < *options.previewStartTick) {
                    return finish(fail("invalid-options", "preview requires a start tick and an end tick"));
                }
                const auto startSample = sampleAtTick(song, *options.previewStartTick, rate);
                const auto endSampleTick = sampleAtTick(song, *options.previewEndTick, rate);
                if (!startSample || !endSampleTick) return finish(fail("sample-overflow", "preview time does not fit in a sample index"));
                emitFrom = static_cast<std::uint64_t>(*startSample);
                emitUntil = static_cast<std::uint64_t>(*endSampleTick) + tailBudget;
            }
            struct RemixTrack {
                const CachedTrack* item{nullptr};
                runtime::WavData dry;
                std::vector<GainSample> automation;
                float constantGain{1.f};
                double pan{0.0};
            };
            std::vector<RemixTrack> remixTracks;
            remixTracks.reserve(cached.size());
            std::uint64_t frames = 0;
            bool dryReady = true;
            for (const auto& item : cached) {
                RemixTrack remix;
                remix.item = &item;
                remix.constantGain = static_cast<float>(item.track->gain);
                remix.pan = item.track->pan;
                for (const auto& point : item.track->gainAutomation) {
                    const auto sample = sampleAtTick(song, point.tick, rate);
                    if (!sample) return finish(fail("sample-overflow", "automation time does not fit in a sample index"));
                    remix.automation.push_back({*sample, static_cast<float>(point.gain)});
                }
                std::stable_sort(remix.automation.begin(), remix.automation.end(), [](const GainSample& left, const GainSample& right) {
                    return left.sample < right.sample;
                });
                std::string wavError;
                runtime::WavData header;
                std::uint32_t dataBytes = 0;
                {
                    std::ifstream probe(item.dry, std::ios::binary);
                    if (!probe || !runtime::readWavHeader(probe, header, dataBytes, wavError) || header.channels != 2 ||
                        header.sampleRate != rate) {
                        dryReady = false;
                        break;
                    }
                }
                const auto totalFrames = static_cast<std::uint64_t>(dataBytes / sizeof(float) / 2);
                frames = std::max(frames, totalFrames);
                remixTracks.push_back(std::move(remix));
            }
            if (dryReady && !remixTracks.empty()) {
                if (!preview) emitUntil = frames;
                else emitUntil = std::min(emitUntil, frames);
                if (emitUntil < emitFrom) return finish(fail("invalid-options", "preview range is empty"));
                const auto needed = emitUntil - emitFrom;
                for (auto& remix : remixTracks) {
                    std::string wavError;
                    if (!runtime::readWavRange(remix.item->dry, remix.dry, emitFrom, needed, wavError) || remix.dry.channels != 2 ||
                        remix.dry.sampleRate != rate) {
                        dryReady = false;
                        break;
                    }
                }
            }
            if (dryReady && !remixTracks.empty()) {
                const bool writeStems = !stemsDirectory.empty();
                if (writeStems) {
                    std::error_code failure;
                    std::filesystem::create_directories(stemsDirectory, failure);
                    if (failure) return finish(fail("output-io", "failed to create the stem directory"));
                }
                std::string ioError;
                auto mix = std::make_unique<runtime::WavStream>();
                if (!mix->open(mixOutput, rate, 2, ioError)) return finish(fail("output-io", ioError));
                std::vector<std::unique_ptr<runtime::WavStream>> stems;
                std::vector<std::filesystem::path> stemPaths;
                std::vector<float> stemPeaks(remixTracks.size(), 0.f);
                if (writeStems) {
                    for (const auto& remix : remixTracks) {
                        auto stem = std::make_unique<runtime::WavStream>();
                        auto path = stemsDirectory / (remix.item->track->id + ".wav");
                        if (!stem->open(path, rate, 2, ioError)) return finish(fail("output-io", ioError));
                        stemPaths.push_back(path);
                        stems.push_back(std::move(stem));
                    }
                }
                std::vector<float> mixed(static_cast<std::size_t>(options.blockSize) * 2, 0.f);
                std::vector<float> stem(static_cast<std::size_t>(options.blockSize) * 2, 0.f);
                float mixPeak = 0.f;
                std::uint64_t mixContentHash = 14695981039346656037ull;
                const auto remixStarted = now();
                for (std::uint64_t frame = emitFrom; frame < emitUntil;) {
                    const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, emitUntil - frame));
                    std::fill(mixed.begin(), mixed.begin() + static_cast<std::size_t>(chunk) * 2, 0.f);
                    for (std::size_t index = 0; index < remixTracks.size(); ++index) {
                        auto& remix = remixTracks[index];
                        for (std::uint32_t offset = 0; offset < chunk; ++offset) {
                            const auto sampleIndex = static_cast<std::int64_t>(frame + offset);
                            const auto local = static_cast<std::size_t>(frame + offset - emitFrom) * 2;
                            float left = 0.f;
                            float right = 0.f;
                            if (local + 1 < remix.dry.interleaved.size()) {
                                left = remix.dry.interleaved[local];
                                right = remix.dry.interleaved[local + 1];
                            }
                            float gainLeft = 0.f;
                            float gainRight = 0.f;
                            equalPower(gainAt(remix.automation, sampleIndex, remix.constantGain), remix.pan, gainLeft, gainRight);
                            const float outLeft = left * gainLeft;
                            const float outRight = right * gainRight;
                            stem[static_cast<std::size_t>(offset) * 2] = outLeft;
                            stem[static_cast<std::size_t>(offset) * 2 + 1] = outRight;
                            mixed[static_cast<std::size_t>(offset) * 2] += outLeft;
                            mixed[static_cast<std::size_t>(offset) * 2 + 1] += outRight;
                            stemPeaks[index] = std::max(stemPeaks[index], std::max(std::fabs(outLeft), std::fabs(outRight)));
                        }
                        if (writeStems && !stems[index]->write(stem.data(), static_cast<std::size_t>(chunk) * 2, ioError)) {
                            return finish(fail("output-io", ioError));
                        }
                    }
                    for (std::uint32_t offset = 0; offset < chunk; ++offset) {
                        mixPeak = std::max(
                            mixPeak,
                            std::max(std::fabs(mixed[static_cast<std::size_t>(offset) * 2]),
                                     std::fabs(mixed[static_cast<std::size_t>(offset) * 2 + 1])));
                    }
                    mixContentHash = mixHash(mixContentHash, mixed.data(), static_cast<std::size_t>(chunk) * 2);
                    if (!mix->write(mixed.data(), static_cast<std::size_t>(chunk) * 2, ioError)) {
                        return finish(fail("output-io", ioError));
                    }
                    frame += chunk;
                }
                mixMs = msBetween(remixStarted, now());
                for (auto& stemFile : stems) {
                    if (!stemFile->commit(ioError)) return finish(fail("output-io", ioError));
                }
                if (!mix->commit(ioError)) return finish(fail("output-io", ioError));
                if (!preview) {
                    std::error_code failure;
                    std::filesystem::create_directories(cachedMix.parent_path(), failure);
                    if (!failure) std::filesystem::copy_file(mixOutput, cachedMix, std::filesystem::copy_options::overwrite_existing, failure);
                }
                SongRenderReport report;
                report.ok = true;
                report.frames = emitUntil - emitFrom;
                report.originSample = emitFrom;
                report.tailFrames = preview ? tailBudget : 0;
                report.peak = mixPeak;
                report.mixHash = hex64(mixContentHash);
                report.message = "remixed";
                report.cacheHit = true;
                report.cacheReason = options.freezeExternal ? "remix-dry-tracks-freeze-external" : "remix-dry-tracks";
                report.timing.mixMs = mixMs;
                report.timing.emittedFrames = report.frames;
                for (std::size_t index = 0; index < remixTracks.size(); ++index) {
                    StemReport stemReport;
                    stemReport.trackId = remixTracks[index].item->track->id;
                    if (writeStems) stemReport.path = stemPaths[index].string();
                    stemReport.peak = stemPeaks[index];
                    stemReport.adapter = remixTracks[index].item->external ? "external-frozen" : "nodsynth";
                    report.stems.push_back(std::move(stemReport));
                }
                return finish(std::move(report));
            }
        }
    }

    const bool writeStems = !stemsDirectory.empty();
    if (writeStems) {
        std::error_code failure;
        std::filesystem::create_directories(stemsDirectory, failure);
        if (failure) return finish(fail("output-io", "failed to create the stem directory"));
    }

    const auto registry = nodes::builtinRegistry();
    std::vector<TrackVoice> voices;
    voices.reserve(song.tracks.size());
    std::uint32_t order = 0;
    for (const auto& track : song.tracks) {
        const bool hasMusic = std::any_of(track.clips.begin(), track.clips.end(), [](const Clip& clip) { return !clip.notes.empty(); }) ||
                              !track.performance.empty();
        if (!safeTrackId(track.id)) return finish(fail("invalid-song", "track id cannot be used as a stem name"));
        TrackVoice voice;
        voice.trackId = track.id;
        voice.soundKey = trackSoundKey(song, track, options);
        voice.pan = track.pan;
        voice.gain = static_cast<float>(track.gain);
        if (!track.gainAutomation.empty()) {
            for (const auto& point : track.gainAutomation) {
                const auto sample = sampleAtTick(song, point.tick, rate);
                if (!sample) return finish(fail("sample-overflow", "automation time does not fit in a sample index"));
                voice.automation.push_back({*sample, static_cast<float>(point.gain)});
            }
            std::stable_sort(voice.automation.begin(), voice.automation.end(), [](const GainSample& left, const GainSample& right) {
                return left.sample < right.sample;
            });
        }
        for (const auto& lane : track.parameterAutomation) {
            for (const auto& point : lane.points) {
                const auto sample = sampleAtTick(song, point.tick, rate);
                if (!sample) return finish(fail("sample-overflow", "parameter automation time does not fit in a sample index"));
                voice.parameters.push_back({*sample, lane.id, point.value});
            }
        }
        std::stable_sort(voice.parameters.begin(), voice.parameters.end(), [](const TrackVoice::ParameterSample& left, const TrackVoice::ParameterSample& right) {
            return left.sample < right.sample;
        });
        if (hasMusic || !track.instrumentId.empty()) {
            if (track.instrumentId.empty()) return finish(fail("missing-patch", "a track with music has no instrument", {{"missing-patch", track.name, 0, -1, track.id}}));
            const auto instrument = std::find_if(song.instruments.begin(), song.instruments.end(), [&](const Instrument& candidate) {
                return candidate.id == track.instrumentId;
            });
            if (instrument == song.instruments.end()) return finish(fail("missing-instrument", "track instrument was not found"));
            if (instrument->kind == InstrumentKind::externalCli || instrument->kind == InstrumentKind::vst3) {
                const auto supported = instrument->kind == InstrumentKind::vst3 ? instrument->adapter == "vst3" : instrument->adapter == "fluidsynth";
                if (!supported) return finish(fail("unknown-adapter", "the external adapter is not supported"));
                const auto tool = std::find_if(options.tools.begin(), options.tools.end(), [&](const SongRenderOptions::ExternalTool& candidate) {
                    return candidate.adapter == instrument->adapter && !candidate.executable.empty();
                });
                if (tool == options.tools.end()) return finish(fail("missing-adapter", instrument->adapter + " was not configured for this render"));
                voice.externalTrack = true;
                voice.adapter = instrument->adapter;
                voice.className = instrument->name;
            } else if (instrument->kind != InstrumentKind::nodsynth) {
                return finish(fail("external-unsupported", "this instrument kind is not available"));
            }
            const auto resource = std::find_if(song.resources.begin(), song.resources.end(), [&](const Resource& candidate) {
                return candidate.id == instrument->resourceId;
            });
            if (resource == song.resources.end() || resource->path.empty()) {
                return finish(fail("missing-patch", "instrument patch was not found", {{"missing-patch", instrument->id, 0, -1, track.id}}));
            }
            const auto patchPath = resolvePath(options.baseDirectory, resource->path);
            if (!resource->hash.empty()) {
                const auto actual = hashFile(patchPath);
                if (actual.empty() || actual != resource->hash) {
                    return finish(fail("resource-hash", "patch file is missing or does not match the song", {{"resource-hash", resource->path, 0, -1, resource->id}}));
                }
            } else if (!std::filesystem::exists(patchPath)) {
                return finish(fail("missing-patch", "patch file was not found", {{"missing-patch", resource->path, 0, -1, track.id}}));
            }
            if (voice.externalTrack) {
                voice.asset = patchPath;
            } else {
                std::string patchError;
                auto graph = render::loadPatch(patchPath, patchError);
                if (!graph) return finish(fail("missing-patch", patchError.empty() ? "failed to load the patch" : patchError));
                runtime::EngineConfig config;
                config.audio.sampleRate = options.sampleRate;
                config.audio.maxFrames = options.blockSize;
                config.audio.voiceCount = 16;
                const auto budgetSeconds = options.tailMode == render::TailMode::fixed ? options.tailSeconds : options.maxTailSeconds;
                config.releaseHoldSeconds = std::max(0.5, budgetSeconds);
                voice.engine = std::make_unique<runtime::Engine>(config);
                const auto compiled = compiler::GraphCompiler{}.compile(*graph, registry);
                if (!compiled.graph) {
                    const auto message = compiled.diagnostics.empty() ? "graph compile failed" : compiled.diagnostics.front().message;
                    return finish(fail("compile-failed", message));
                }
                auto prepared = runtime::preparePlan(
                    *compiled.graph, registry, nodes::builtinImplementations(), voice.engine->config(), voice.engine->voices(),
                    voice.engine->parameterSmoothSeconds());
                if (!prepared.plan || !voice.engine->stage(std::move(prepared.plan)).accepted) {
                    return finish(fail("prepare-failed", prepared.message.empty() ? "failed to prepare the patch" : prepared.message));
                }
            }
        }
        bool sustainDown[16]{};
        for (const auto& clip : track.clips) {
            for (const auto& note : clip.notes) {
                const auto startTick = static_cast<std::uint64_t>(clip.startTick) + note.tick;
                const auto stopTick = startTick + note.duration;
                if (startTick > 0xffffffffu || stopTick > 0xffffffffu) return finish(fail("invalid-song", "a note does not fit in the timeline"));
                const auto onSample = sampleAtTick(song, static_cast<std::uint32_t>(startTick), rate);
                const auto offSample = sampleAtTick(song, static_cast<std::uint32_t>(stopTick), rate);
                if (!onSample || !offSample) return finish(fail("sample-overflow", "note time does not fit in a sample index"));
                runtime::MidiEvent on;
                on.type = runtime::MidiType::noteOn;
                on.channel = note.channel;
                on.data1 = note.pitch;
                on.data2 = note.velocity;
                voice.events.push_back({*onSample, 2, order++, on});
                runtime::MidiEvent off;
                off.type = runtime::MidiType::noteOff;
                off.channel = note.channel;
                off.data1 = note.pitch;
                voice.events.push_back({*offSample, note.duration == 0 ? 3 : 0, order++, off});
            }
        }
        for (const auto& performance : track.performance) {
            runtime::MidiEvent midi{};
            if (!toEngine(performance, midi)) continue;
            const auto sample = sampleAtTick(song, performance.tick, rate);
            if (!sample) return finish(fail("sample-overflow", "controller time does not fit in a sample index"));
            voice.events.push_back({*sample, 1, order++, midi});
            if (performance.kind == midi::EventKind::controlChange && performance.data1 == 64) {
                sustainDown[performance.channel] = performance.data2 >= 64;
            }
            if (performance.kind == midi::EventKind::controlChange && performance.data1 == 120) sustainDown[performance.channel] = false;
        }
        const auto ending = sampleAtTick(song, endTick(song), rate);
        if (!ending) return finish(fail("sample-overflow", "song length does not fit in a sample index"));
        for (std::uint8_t channel = 0; channel < 16; ++channel) {
            if (!sustainDown[channel]) continue;
            voice.events.push_back({*ending, 0, order++, {0, runtime::MidiType::sustain, channel, 64, 0}});
        }
        std::stable_sort(voice.events.begin(), voice.events.end(), [](const TimedEvent& left, const TimedEvent& right) {
            if (left.sample != right.sample) return left.sample < right.sample;
            if (left.priority != right.priority) return left.priority < right.priority;
            return left.order < right.order;
        });
        voices.push_back(std::move(voice));
    }

    std::int64_t endSample = 0;
    const auto mappedEnd = sampleAtTick(song, endTick(song), rate);
    if (!mappedEnd) return finish(fail("sample-overflow", "song length does not fit in a sample index"));
    endSample = *mappedEnd;
    for (const auto& voice : voices) {
        for (const auto& event : voice.events) endSample = std::max(endSample, event.sample);
        std::uint32_t blockEvents = 0;
        std::int64_t blockSample = -1;
        for (const auto& event : voice.events) {
            const auto blockStart = event.sample - event.sample % options.blockSize;
            if (blockStart != blockSample) {
                blockEvents = 0;
                blockSample = blockStart;
            }
            if (++blockEvents > options.maxEventsPerBlock) {
                return finish(fail("event-density", "a render block contains more MIDI events than the engine can play"));
            }
        }
    }

    prepareMs = msBetween(started, now());
    const auto budgetSeconds = options.tailMode == render::TailMode::fixed ? options.tailSeconds : options.maxTailSeconds;
    const auto tailBudget = static_cast<std::uint64_t>(std::llround(budgetSeconds * options.sampleRate));
    const bool haveExternal = std::any_of(voices.begin(), voices.end(), [](const TrackVoice& voice) { return voice.externalTrack; });
    std::filesystem::path work = mixOutput;
    work += ".work";
    if (haveExternal) {
        std::error_code failure;
        std::filesystem::create_directories(work, failure);
        if (failure) return finish(fail("output-io", "failed to create the external render directory"));
    }
    struct WorkCleanup {
        std::filesystem::path path;
        bool active{false};
        ~WorkCleanup() {
            if (!active) return;
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{work, haveExternal};
    externalStarted = now();
    externalRunning = haveExternal;
    for (auto& voice : voices) {
        if (!voice.externalTrack) continue;
        const auto tool = std::find_if(options.tools.begin(), options.tools.end(), [&](const SongRenderOptions::ExternalTool& candidate) {
            return candidate.adapter == voice.adapter && !candidate.executable.empty();
        });
        if (tool == options.tools.end()) return finish(fail("missing-adapter", voice.adapter + " was not configured for this render"));
        std::string exportError;
        const auto midi = exportTrackMidi(song, voice.trackId, exportError);
        if (midi.empty()) return finish(fail("external-failed", exportError.empty() ? "failed to export the external MIDI" : exportError));
        const auto midiPath = work / (voice.trackId + ".mid");
        const auto wavPath = work / (voice.trackId + ".wav");
        {
            std::ofstream output(midiPath, std::ios::binary | std::ios::trunc);
            if (!output) return finish(fail("output-io", "failed to write the external MIDI file"));
            output.write(reinterpret_cast<const char*>(midi.data()), static_cast<std::streamsize>(midi.size()));
            if (!output) return finish(fail("output-io", "failed to write the external MIDI file"));
        }
        ProcessRequest request;
        request.timeoutMs = tool->timeoutMs;
        if (!voice.parameters.empty() && voice.adapter != "vst3") {
            return finish(fail("unsupported-automation", "parameter automation is available for VST3 instruments"));
        }
        const auto automationPath = work / (voice.trackId + ".automation.json");
        if (voice.adapter == "vst3") {
            persist::Json document = persist::Json::object();
            persist::Json points = persist::Json::array();
            for (const auto& point : voice.parameters) {
                persist::Json item = persist::Json::object();
                item.set("sample", persist::Json::number(static_cast<double>(point.sample)));
                item.set("parameter", persist::Json::string(point.name));
                item.set("value", persist::Json::number(point.value));
                points.push(std::move(item));
            }
            document.set("points", std::move(points));
            std::ofstream automationFile(automationPath, std::ios::binary | std::ios::trunc);
            const auto automationText = document.dump();
            if (!automationFile) return finish(fail("output-io", "failed to write parameter automation"));
            automationFile.write(automationText.data(), static_cast<std::streamsize>(automationText.size()));
            if (!automationFile) return finish(fail("output-io", "failed to write parameter automation"));
            request.arguments = {
                utf8Path(tool->executable), utf8Path(voice.asset), voice.className.empty() ? "-" : voice.className, utf8Path(midiPath),
                utf8Path(wavPath), std::to_string(rate), std::to_string(options.blockSize), std::to_string(budgetSeconds),
                utf8Path(automationPath)};
        } else {
            request.arguments = {
                utf8Path(tool->executable), "-q", "-ni", "-R", "0", "-C", "0", "-r", std::to_string(rate), "-T", "wav", "-O", "float", "-g", "1",
                "-o", "synth.cpu-cores=1", "-F", utf8Path(wavPath), utf8Path(voice.asset), utf8Path(midiPath)};
        }
        const auto process = runProcess(request);
        if (!process.started) return finish(fail("missing-adapter", process.message.empty() ? "failed to start the external renderer" : process.message));
        if (process.timedOut) return finish(fail("external-timeout", process.message));
        if (process.exitCode != 0) return finish(fail("external-failed", process.message.empty() ? "the external renderer failed" : process.message));
        if (voice.adapter == "vst3") {
            std::ifstream latencyFile(wavPath.string() + ".latency");
            latencyFile >> voice.latencySamples;
        }
        DecodedWav decoded;
        std::string decodeError;
        if (!decodeWav(wavPath, rate, decoded, decodeError)) return finish(fail("external-format", decodeError));
        voice.external = std::move(decoded.interleaved);
        const auto drop = static_cast<std::size_t>(voice.latencySamples) * 2;
        if (drop > 0 && drop < voice.external.size()) voice.external.erase(voice.external.begin(), voice.external.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    if (haveExternal) externalMs = msBetween(externalStarted, now());
    externalRunning = false;
    std::uint64_t externalFrames = 0;
    for (const auto& voice : voices) externalFrames = std::max(externalFrames, static_cast<std::uint64_t>(voice.external.size() / 2));
    const auto musicalFrames = static_cast<std::uint64_t>(endSample);
    std::uint64_t targetFrames = musicalFrames + tailBudget;
    if (externalFrames > targetFrames) targetFrames = externalFrames;
    if (targetFrames > (0xffffffffu - 36u) / 8u) {
        return finish(fail("riff-limit", "rendered audio exceeds the RIFF size limit"));
    }

    std::string ioError;
    const auto writeOpenStarted = now();
    auto mix = std::make_unique<runtime::WavStream>();
    if (!mix->open(mixOutput, rate, 2, ioError)) return finish(fail("output-io", ioError));
    std::vector<std::filesystem::path> committed;
    for (auto& voice : voices) {
        if (writeStems) {
            voice.stem = std::make_unique<runtime::WavStream>();
            voice.stemPath = stemsDirectory / (voice.trackId + ".wav");
            if (!voice.stem->open(voice.stemPath, rate, 2, ioError)) return finish(fail("output-io", ioError));
        }
        if (options.useCache && !preview && !voice.soundKey.empty() &&
            (!voice.externalTrack || options.freezeExternal) && (options.quality.empty() || options.quality == "final")) {
            std::error_code failure;
            const auto path = dryPath(cacheDir, voice.soundKey);
            std::filesystem::create_directories(path.parent_path(), failure);
            if (!failure) {
                voice.dry = std::make_unique<runtime::WavStream>();
                voice.dryFile = path;
                if (!voice.dry->open(voice.dryFile, rate, 2, ioError)) return finish(fail("output-io", ioError));
            }
        }
    }
    writeMs += msBetween(writeOpenStarted, now());

    std::uint64_t emitFrom = 0;
    std::uint64_t emitUntil = ~std::uint64_t{0};
    if (preview) {
        if (!options.previewStartTick || !options.previewEndTick || *options.previewEndTick < *options.previewStartTick) {
            return finish(fail("invalid-options", "preview requires a start tick and an end tick"));
        }
        const auto startSample = sampleAtTick(song, *options.previewStartTick, rate);
        const auto endSampleTick = sampleAtTick(song, *options.previewEndTick, rate);
        if (!startSample || !endSampleTick) return finish(fail("sample-overflow", "preview time does not fit in a sample index"));
        emitFrom = static_cast<std::uint64_t>(*startSample);
        emitUntil = static_cast<std::uint64_t>(*endSampleTick) + tailBudget;
        if (emitUntil < emitFrom || emitUntil > (0xffffffffu - 36u) / 8u) {
            return finish(fail("riff-limit", "preview audio exceeds the RIFF size limit"));
        }
    }
    std::uint64_t writtenFrames = 0;
    std::vector<float> left(options.blockSize, 0.f);
    std::vector<float> right(options.blockSize, 0.f);
    std::vector<float> stem(static_cast<std::size_t>(options.blockSize) * 2, 0.f);
    std::vector<float> mixed(static_cast<std::size_t>(options.blockSize) * 2, 0.f);
    std::uint64_t mixContentHash = 14695981039346656037ull;
    float mixPeak = 0.f;
    const auto renderChunk = [&](std::uint64_t rendered, std::uint32_t chunk) {
        std::fill(mixed.begin(), mixed.begin() + static_cast<std::size_t>(chunk) * 2, 0.f);
        float blockPeak = 0.f;
        double chunkDsp = 0;
        double chunkMix = 0;
        double chunkWrite = 0;
        for (auto& voice : voices) {
            std::vector<runtime::MidiEvent> block;
            const auto blockEnd = static_cast<std::int64_t>(rendered + chunk);
            while (voice.cursor < voice.events.size() && voice.events[voice.cursor].sample < blockEnd) {
                if (voice.events[voice.cursor].sample >= static_cast<std::int64_t>(rendered)) {
                    auto event = voice.events[voice.cursor].midi;
                    event.sampleOffset = static_cast<std::uint32_t>(voice.events[voice.cursor].sample - static_cast<std::int64_t>(rendered));
                    block.push_back(event);
                }
                ++voice.cursor;
            }
            const auto dspStarted = now();
            std::fill(left.begin(), left.begin() + chunk, 0.f);
            std::fill(right.begin(), right.begin() + chunk, 0.f);
            if (voice.externalTrack) {
                for (std::uint32_t frame = 0; frame < chunk; ++frame) {
                    if (voice.externalCursor + 1 >= voice.external.size()) break;
                    left[frame] = voice.external[voice.externalCursor++];
                    right[frame] = voice.external[voice.externalCursor++];
                }
            } else if (voice.engine) {
                float* outputs[] = {left.data(), right.data()};
                voice.engine->process(outputs, 2, chunk, block);
                voice.engine->reclaim();
            }
            const auto mixStarted = now();
            chunkDsp += msBetween(dspStarted, mixStarted);
            if (voice.dry) {
                for (std::uint32_t frame = 0; frame < chunk; ++frame) {
                    stem[static_cast<std::size_t>(frame) * 2] = left[frame];
                    stem[static_cast<std::size_t>(frame) * 2 + 1] = right[frame];
                }
                if (!voice.dry->write(stem.data(), static_cast<std::size_t>(chunk) * 2, ioError)) return -1.f;
            }
            for (std::uint32_t frame = 0; frame < chunk; ++frame) {
                const auto sample = static_cast<std::int64_t>(rendered + frame);
                float gainLeft = 0.f;
                float gainRight = 0.f;
                equalPower(gainAt(voice.automation, sample, voice.gain), voice.pan, gainLeft, gainRight);
                const float sampleLeft = left[frame] * gainLeft;
                const float sampleRight = right[frame] * gainRight;
                stem[static_cast<std::size_t>(frame) * 2] = sampleLeft;
                stem[static_cast<std::size_t>(frame) * 2 + 1] = sampleRight;
                mixed[static_cast<std::size_t>(frame) * 2] += sampleLeft;
                mixed[static_cast<std::size_t>(frame) * 2 + 1] += sampleRight;
            }
            const auto writeStarted = now();
            chunkMix += msBetween(mixStarted, writeStarted);
            const auto chunkBegin = rendered;
            const auto chunkEnd = rendered + chunk;
            const auto keepBegin = std::max(chunkBegin, emitFrom);
            const auto keepEnd = std::min(chunkEnd, emitUntil);
            if (keepEnd > keepBegin) {
                const auto first = static_cast<std::size_t>(keepBegin - chunkBegin);
                const auto count = static_cast<std::size_t>(keepEnd - keepBegin);
                float stemPeak = 0.f;
                for (std::size_t frame = 0; frame < count; ++frame) {
                    stemPeak = std::max(
                        stemPeak, std::max(std::fabs(stem[(first + frame) * 2]), std::fabs(stem[(first + frame) * 2 + 1])));
                }
                voice.peak = std::max(voice.peak, stemPeak);
                voice.hash = mixHash(voice.hash, stem.data() + first * 2, count * 2);
                blockPeak = std::max(blockPeak, stemPeak);
                if (voice.stem && !voice.stem->write(stem.data() + first * 2, count * 2, ioError)) return -1.f;
            }
            chunkWrite += msBetween(writeStarted, now());
        }
        const auto chunkBegin = rendered;
        const auto chunkEnd = rendered + chunk;
        const auto keepBegin = std::max(chunkBegin, emitFrom);
        const auto keepEnd = std::min(chunkEnd, emitUntil);
        const auto mixWriteStarted = now();
        if (keepEnd > keepBegin) {
            const auto first = static_cast<std::size_t>(keepBegin - chunkBegin);
            const auto count = static_cast<std::size_t>(keepEnd - keepBegin);
            for (std::size_t frame = 0; frame < count; ++frame) {
                blockPeak = std::max(blockPeak, std::max(std::fabs(mixed[(first + frame) * 2]), std::fabs(mixed[(first + frame) * 2 + 1])));
            }
            mixPeak = std::max(mixPeak, blockPeak);
            mixContentHash = mixHash(mixContentHash, mixed.data() + first * 2, count * 2);
            writtenFrames += count;
            if (!mix->write(mixed.data() + first * 2, count * 2, ioError)) return -1.f;
        }
        chunkWrite += msBetween(mixWriteStarted, now());
        if (chunkEnd <= emitFrom) {
            prerollMs += chunkDsp + chunkMix;
        } else if (rendered < emitFrom) {
            const double prerollFrac = static_cast<double>(emitFrom - rendered) / static_cast<double>(chunk);
            prerollMs += prerollFrac * (chunkDsp + chunkMix);
            dspMs += (1.0 - prerollFrac) * chunkDsp;
            mixMs += (1.0 - prerollFrac) * chunkMix;
            writeMs += chunkWrite;
        } else {
            dspMs += chunkDsp;
            mixMs += chunkMix;
            writeMs += chunkWrite;
        }
        return blockPeak;
    };

    auto discard = [&]() {
        mix->abort();
        for (auto& voice : voices) {
            if (voice.stem) voice.stem->abort();
            if (voice.dry) voice.dry->abort();
        }
        std::error_code ignored;
        for (const auto& path : committed) std::filesystem::remove(path, ignored);
    };

    std::uint64_t rendered = 0;
    if (preview) {
        while (rendered < emitUntil) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, emitUntil - rendered));
            if (renderChunk(rendered, chunk) < 0.f) {
                discard();
                return finish(fail("output-io", ioError));
            }
            rendered += chunk;
        }
        const auto previewEnd = emitUntil > tailBudget ? emitUntil - tailBudget : 0;
        const auto previewTail = rendered > previewEnd ? rendered - previewEnd : 0;
        const auto commitStarted = now();
        for (auto& voice : voices) {
            if (!voice.stem) continue;
            if (!voice.stem->commit(ioError)) {
                discard();
                return finish(fail("output-io", ioError));
            }
            committed.push_back(voice.stemPath);
        }
        if (!mix->commit(ioError)) {
            discard();
            return finish(fail("output-io", ioError));
        }
        writeMs += msBetween(commitStarted, now());
        renderedFrames = rendered;
        emittedFrames = writtenFrames;
        SongRenderReport report;
        report.ok = true;
        report.frames = writtenFrames;
        report.timing.renderedFrames = rendered;
        report.timing.emittedFrames = writtenFrames;
        report.originSample = emitFrom;
        report.tailFrames = std::min<std::uint64_t>(previewTail, writtenFrames);
        report.peak = mixPeak;
        report.mixHash = hex64(mixContentHash);
        report.message = "rendered";
        report.diagnostics = song.diagnostics;
        for (const auto& voice : voices) {
            StemReport stem;
            stem.trackId = voice.trackId;
            stem.path = voice.stemPath.string();
            stem.hash = hex64(voice.hash);
            stem.peak = voice.peak;
            stem.adapter = voice.adapter;
            stem.latencySamples = voice.latencySamples;
            report.stems.push_back(std::move(stem));
        }
        return finish(std::move(report));
    }
    while (rendered < musicalFrames) {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, musicalFrames - rendered));
        if (renderChunk(rendered, chunk) < 0.f) {
            discard();
            return finish(fail("output-io", ioError));
        }
        rendered += chunk;
    }
    std::uint64_t tailFrames = 0;
    bool tailTruncated = false;
    if (haveExternal) {
        while (rendered < targetFrames) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, targetFrames - rendered));
            if (renderChunk(rendered, chunk) < 0.f) {
                discard();
                return finish(fail("output-io", ioError));
            }
            rendered += chunk;
            if (rendered > musicalFrames) tailFrames = rendered - musicalFrames;
        }
    } else if (options.tailMode == render::TailMode::fixed) {
        while (tailFrames < tailBudget) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, tailBudget - tailFrames));
            if (renderChunk(rendered, chunk) < 0.f) {
                discard();
                return finish(fail("output-io", ioError));
            }
            rendered += chunk;
            tailFrames += chunk;
        }
    } else {
        bool hot = true;
        while (tailFrames < tailBudget && hot) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.blockSize, tailBudget - tailFrames));
            const auto peak = renderChunk(rendered, chunk);
            if (peak < 0.f) {
                discard();
                return finish(fail("output-io", ioError));
            }
            hot = peak >= static_cast<float>(options.tailThreshold);
            rendered += chunk;
            tailFrames += chunk;
        }
        tailTruncated = hot && tailFrames >= tailBudget;
    }
    const auto commitStarted = now();
    for (auto& voice : voices) {
        if (voice.stem) {
            if (!voice.stem->commit(ioError)) {
                discard();
                return finish(fail("output-io", ioError));
            }
            committed.push_back(voice.stemPath);
        }
        if (voice.dry) {
            if (!voice.dry->commit(ioError)) {
                discard();
                return finish(fail("output-io", ioError));
            }
        }
    }
    if (!mix->commit(ioError)) {
        discard();
        return finish(fail("output-io", ioError));
    }
    writeMs += msBetween(commitStarted, now());
    renderedFrames = rendered;
    emittedFrames = writtenFrames;

    SongRenderReport report;
    report.ok = true;
    report.frames = writtenFrames;
    report.timing.renderedFrames = rendered;
    report.timing.emittedFrames = writtenFrames;
    report.originSample = emitFrom;
    report.tailFrames = tailFrames;
    report.tailTruncated = tailTruncated;
    report.peak = mixPeak;
    report.mixHash = hex64(mixContentHash);
    report.message = "rendered";
    report.diagnostics = song.diagnostics;
    if (options.mode == midi::RenderMode::loose) {
        for (const auto& event : song.preserved) {
            if (!event.affectsSound) continue;
            report.diagnostics.push_back(
                {"unsupported-event", std::string(midi::eventKindName(event.kind)) + " was preserved and not played", event.tick, event.sourceTrack});
        }
    }
    for (const auto& voice : voices) {
        StemReport stem;
        stem.trackId = voice.trackId;
        stem.path = voice.stemPath.string();
        stem.hash = hex64(voice.hash);
        stem.peak = voice.peak;
        stem.adapter = voice.adapter;
        stem.latencySamples = voice.latencySamples;
        if (voice.adapter == "fluidsynth") {
            report.diagnostics.push_back(
                {"non-deterministic", "FluidSynth output is not bit-exact across machines or versions", 0, -1, voice.trackId});
        }
        report.stems.push_back(std::move(stem));
    }
    if (options.useCache && !preview) {
        std::error_code failure;
        std::filesystem::create_directories(cachedMix.parent_path(), failure);
        if (!failure) {
            std::filesystem::copy_file(mixOutput, cachedMix, std::filesystem::copy_options::overwrite_existing, failure);
            if (!failure) {
                report.cacheHit = false;
                report.cacheReason = "stored-finished-mix";
            }
        }
        const bool storedDry = std::any_of(voices.begin(), voices.end(), [](const TrackVoice& voice) { return !voice.dryFile.empty(); });
        if (storedDry) {
            if (report.cacheReason == "stored-finished-mix") report.cacheReason = "stored-finished-mix-and-dry-tracks";
            else report.cacheReason = "stored-dry-tracks";
        }
    }
    return finish(std::move(report));
}

CompareReport compareWav(const std::filesystem::path& aPath, const std::filesystem::path& bPath, const CompareOptions& options) {
    CompareReport report;
    report.a = analyzeWav(aPath, options.silenceThreshold);
    report.b = analyzeWav(bPath, options.silenceThreshold);
    if (!report.a.ok || !report.b.ok) {
        report.message = !report.a.ok ? report.a.message : report.b.message;
        return report;
    }
    runtime::WavData a;
    runtime::WavData b;
    std::string error;
    if (!runtime::readWav(aPath, a, error) || !runtime::readWav(bPath, b, error)) {
        report.message = error.empty() ? "failed to read comparison WAV" : error;
        return report;
    }
    report.peakDelta = static_cast<double>(report.b.peak) - static_cast<double>(report.a.peak);
    report.rmsDelta = report.b.rms - report.a.rms;
    if (report.a.loudnessLufs && report.b.loudnessLufs) report.lufsDelta = *report.b.loudnessLufs - *report.a.loudnessLufs;
    if (options.matchLoudness) {
        if (!report.a.loudnessLufs || !report.b.loudnessLufs) {
            report.message = "loudness matching unavailable for this format";
            return report;
        }
        const double gainDb = *report.a.loudnessLufs - *report.b.loudnessLufs;
        const float gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
        for (float& sample : b.interleaved) sample *= gain;
        const auto temp = std::filesystem::temp_directory_path() / "nodsynth-compare-matched.wav";
        if (!runtime::writeWav(temp, b, error)) {
            report.message = error;
            return report;
        }
        report.matchedB = analyzeWav(temp, options.silenceThreshold);
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        report.loudnessMatched = true;
        report.peakDelta = static_cast<double>(report.matchedB.peak) - static_cast<double>(report.a.peak);
        report.rmsDelta = report.matchedB.rms - report.a.rms;
        if (report.a.loudnessLufs && report.matchedB.loudnessLufs) {
            report.lufsDelta = *report.matchedB.loudnessLufs - *report.a.loudnessLufs;
        }
    }
    report.ok = true;
    report.message = "compared";
    return report;
}

persist::Json compareJson(const CompareReport& report) {
    persist::Json json = persist::Json::object();
    json.set("status", persist::Json::string(report.ok ? "ok" : "rejected"));
    json.set("message", persist::Json::string(report.message));
    json.set("a", analysisJson(report.a));
    json.set("b", analysisJson(report.b));
    if (report.loudnessMatched) json.set("matchedB", analysisJson(report.matchedB));
    json.set("peakDelta", persist::Json::number(report.peakDelta));
    json.set("rmsDelta", persist::Json::number(report.rmsDelta));
    if (report.lufsDelta) json.set("lufsDelta", persist::Json::number(*report.lufsDelta));
    json.set("loudnessMatched", persist::Json::boolean(report.loudnessMatched));
    return json;
}
} // namespace nodsynth::song
