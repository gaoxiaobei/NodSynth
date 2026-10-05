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
    auto finish = [&](SongRenderReport report) {
        report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
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
    std::uint64_t externalFrames = 0;
    for (const auto& voice : voices) externalFrames = std::max(externalFrames, static_cast<std::uint64_t>(voice.external.size() / 2));
    const auto musicalFrames = static_cast<std::uint64_t>(endSample);
    std::uint64_t targetFrames = musicalFrames + tailBudget;
    if (externalFrames > targetFrames) targetFrames = externalFrames;
    if (targetFrames > (0xffffffffu - 36u) / 8u) {
        return finish(fail("riff-limit", "rendered audio exceeds the RIFF size limit"));
    }

    std::string ioError;
    auto mix = std::make_unique<runtime::WavStream>();
    if (!mix->open(mixOutput, rate, 2, ioError)) return finish(fail("output-io", ioError));
    std::vector<std::filesystem::path> committed;
    for (auto& voice : voices) {
        if (!writeStems) continue;
        voice.stem = std::make_unique<runtime::WavStream>();
        voice.stemPath = stemsDirectory / (voice.trackId + ".wav");
        if (!voice.stem->open(voice.stemPath, rate, 2, ioError)) return finish(fail("output-io", ioError));
    }

    const bool preview = options.previewStartTick.has_value() || options.previewEndTick.has_value();
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
        }
        const auto chunkBegin = rendered;
        const auto chunkEnd = rendered + chunk;
        const auto keepBegin = std::max(chunkBegin, emitFrom);
        const auto keepEnd = std::min(chunkEnd, emitUntil);
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
        return blockPeak;
    };

    auto discard = [&]() {
        mix->abort();
        for (auto& voice : voices) {
            if (voice.stem) voice.stem->abort();
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
        SongRenderReport report;
        report.ok = true;
        report.frames = writtenFrames;
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

    SongRenderReport report;
    report.ok = true;
    report.frames = writtenFrames;
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
    return finish(std::move(report));
}
} // namespace nodsynth::song
