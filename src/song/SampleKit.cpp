#include <nodsynth/song/Sampler.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>

namespace nodsynth::song {
bool createSampleKit(const std::filesystem::path& directory, std::string& error, bool punchKick) {
    if (std::filesystem::exists(directory)) { error = "sample kit output directory already exists"; return false; }
    std::error_code ec;
    if (!std::filesystem::create_directories(directory, ec) || ec) { error = "failed to create sample kit directory"; return false; }
    const auto rollback = [&] { std::filesystem::remove_all(directory, ec); };
    struct Drum { const char* name; int note, choke; double duration; };
    const Drum drums[]{{"kick",36,0,.6}, {"snare",38,0,.35}, {"clap",39,0,.3},
        {"closed-hat",42,1,.12}, {"open-hat",46,1,.7}, {"percussion",49,0,.25}};
    auto manifest = persist::Json::object();
    manifest.set("format", persist::Json::string("nod-sample-kit"));
    manifest.set("version", persist::Json::number(1));
    manifest.set("name", persist::Json::string("Native Electronic One-Shots"));
    manifest.set("license", persist::Json::string("CC0-1.0"));
    manifest.set("source", persist::Json::string("Original procedural synthesis, NodSynth sample-kit generator v1; no external recordings"));
    if (punchKick) {
        manifest.set("name", persist::Json::string("Native Electronic One-Shots - Punch Kick"));
        manifest.set("edition", persist::Json::number(2));
        manifest.set("source", persist::Json::string("Original procedural synthesis, NodSynth punch-kick edition v2; no external recordings"));
    }
    manifest.set("auditionStatus", persist::Json::string("unheard"));
    auto samples = persist::Json::array();
    constexpr std::uint32_t rate = 48000;
    std::uint32_t random = 1979;
    for (std::size_t drumIndex = 0; drumIndex < std::size(drums); ++drumIndex) {
        const auto& drum = drums[drumIndex];
        // Two independently seeded recordings for deterministic round robin.
        for (int alternate = 0; alternate < 2; ++alternate) {
            runtime::WavData audio; audio.sampleRate = rate; audio.channels = 1;
            audio.interleaved.resize(static_cast<std::size_t>(drum.duration * rate));
            double phase = 0, low = 0, previousNoise = 0, clickLow = 0;
            for (std::size_t frame = 0; frame < audio.interleaved.size(); ++frame) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                const double noise = static_cast<double>(random) / 2147483648.0 - 1;
                const double time = static_cast<double>(frame) / rate;
                low += .25 * (noise - low);
                const double high = (noise - previousNoise) * .5; previousNoise = noise;
                const double attack = std::min(1.0, time / (punchKick && drumIndex == 0 ? .0003 : .0008));
                double sample = 0;
                if (drumIndex == 0) {
                    if (punchKick) {
                        const double frequency = 55 + (alternate ? 255 : 245) * std::exp(-time / .018);
                        phase += 2 * std::numbers::pi * frequency / rate;
                        clickLow += (1 - std::exp(-2 * std::numbers::pi * 4500 / rate)) * (high - clickLow);
                        const double body = std::exp(-std::max(0., time - .006) / .17);
                        sample = .76 * std::sin(phase) * body + .12 * std::sin(2 * phase) * std::exp(-time / .045) +
                            .065 * clickLow * std::exp(-time / .004);
                    } else {
                        phase += 2 * std::numbers::pi * (48 + (alternate ? 126 : 120) * std::exp(-time / .025)) / rate;
                        sample = .7 * std::sin(phase) * std::exp(-time / .14) + .08 * high * std::exp(-time / .008);
                    }
                } else if (drumIndex == 1) {
                    phase += 2 * std::numbers::pi * (alternate ? 184 : 180) / rate;
                    sample = .25 * std::sin(phase) * std::exp(-time / .06) + .5 * (noise - low) * std::exp(-time / .08);
                } else if (drumIndex == 2) {
                    double envelope = std::exp(-time / .06) * .5;
                    for (const double offset : {.008, .016, .024}) if (time >= offset) envelope += std::exp(-(time - offset) / .006) * .2;
                    sample = (noise - low) * envelope;
                } else if (drumIndex == 3 || drumIndex == 4) {
                    sample = high * std::exp(-time / (drumIndex == 3 ? .024 : .16)) * .55;
                } else {
                    phase += 2 * std::numbers::pi * (300 + 350 * std::exp(-time / .02)) / rate;
                    sample = .4 * std::sin(phase) * std::exp(-time / .05) + .08 * high * std::exp(-time / .02);
                }
                const double endFade = std::min(1.0, (audio.interleaved.size() - 1 - frame) / (rate * .005));
                audio.interleaved[frame] = static_cast<float>(sample * attack * endFade);
            }
            const auto name = std::string(drum.name) + "-" + std::to_string(alternate + 1) + ".wav";
            if (!runtime::writeWav(directory / name, audio, error)) { rollback(); return false; }
            SampleLayer layer; layer.resourceId = name; layer.rootNote = layer.lowNote = layer.highNote = drum.note; layer.chokeGroup = drum.choke;
            auto item = sampleLayersJson({layer}).asArray()[0];
            item.set("path", persist::Json::string(name));
            item.set("hash", persist::Json::string(hashFile(directory / name)));
            samples.push(std::move(item));
        }
    }
    manifest.set("samples", std::move(samples));
    std::ofstream output(directory / "kit.json", std::ios::binary);
    const auto text = manifest.dump(); output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) { error = "failed to save sample kit manifest"; rollback(); return false; }
    return true;
}

bool bindSampleKit(SongDocument& song, const std::string& trackId, const std::filesystem::path& manifestPath, std::string& error) {
    auto track = std::find_if(song.tracks.begin(), song.tracks.end(), [&](const auto& t) { return t.id == trackId; });
    if (track == song.tracks.end()) { error = "sample kit track was not found"; return false; }
    std::error_code ec;
    const auto size = std::filesystem::file_size(manifestPath, ec);
    if (ec || size > 1024 * 1024) { error = "sample kit manifest is missing or exceeds 1 MiB"; return false; }
    std::ifstream input(manifestPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    const auto json = persist::Json::parse(text, error);
    if (!json || !json->find("format") || json->find("format")->asString() != "nod-sample-kit" ||
        !json->find("version") || json->find("version")->asNumber() != 1 || !json->find("samples") ||
        !json->find("license") || json->find("license")->asString().empty() || !json->find("source") || json->find("source")->asString().empty()) {
        error = "sample kit requires format nod-sample-kit, version 1, samples, license and source"; return false;
    }
    Instrument instrument;
    instrument.id = "sampler-" + trackId; instrument.kind = InstrumentKind::sampler; instrument.adapter = "sampler-v1";
    instrument.name = json->find("name") ? json->find("name")->asString() : "One-shot kit";
    if (!parseSampleLayers(*json->find("samples"), instrument.samples, error)) return false;
    auto resources = song.resources;
    for (std::size_t index = 0; index < instrument.samples.size(); ++index) {
        const auto& item = json->find("samples")->asArray()[index];
        if (!item.find("path") || !item.find("hash") || item.find("hash")->asString().empty()) { error = "kit sample requires path and hash"; return false; }
        const auto file = std::filesystem::absolute(resolveResourcePath(manifestPath.parent_path(), item.find("path")->asString()));
        const auto actual = hashFile(file);
        if (actual.empty() || actual != item.find("hash")->asString()) { error = "sample kit file is missing or changed"; return false; }
        Resource resource;
        resource.id = "sample-" + actual; resource.kind = "sample"; resource.hash = actual;
        const auto utf8 = file.u8string(); resource.path = {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        resource.license = json->find("license")->asString(); resource.source = json->find("source")->asString();
        const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto& r) { return r.id == resource.id; });
        if (found == resources.end()) resources.push_back(resource);
        else if (found->kind != "sample" || found->hash != actual) { error = "sample resource id collision"; return false; }
        instrument.samples[index].resourceId = resource.id;
    }
    auto next = song; next.resources = std::move(resources);
    if (!validateSampleLayers(instrument, next, error)) return false;
    const auto previous = std::find_if(next.instruments.begin(), next.instruments.end(), [&](const auto& i) { return i.id == instrument.id; });
    if (previous == next.instruments.end()) next.instruments.push_back(instrument);
    else if (previous->kind != InstrumentKind::sampler) { error = "sample instrument id collision"; return false; }
    else *previous = instrument;
    for (auto& t : next.tracks) if (t.id == trackId) t.instrumentId = instrument.id;
    song = std::move(next); return true;
}
}
