#include <nodsynth/song/AudioExport.h>
#include <nodsynth/song/SongDocument.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace nodsynth::song {
namespace {
bool replace(const std::filesystem::path& temporary, const std::filesystem::path& target, std::string& error) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
    std::error_code failure;
    std::filesystem::rename(temporary, target, failure);
    if (!failure) return true;
#endif
    error = "failed to replace exported file";
    std::error_code ignored; std::filesystem::remove(temporary, ignored); return false;
}

persist::Json readRecords(const std::filesystem::path& path, std::string& error) {
    if (!std::filesystem::exists(path)) return persist::Json::array();
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "cannot read audition records"; return persist::Json::null(); }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto parsed = persist::Json::parse(text, error);
    if (!parsed || parsed->kind() != persist::Json::Kind::array) {
        if (error.empty()) error = "invalid audition records"; return persist::Json::null();
    }
    return *parsed;
}
} // namespace

bool writePcm16(const std::filesystem::path& path, const runtime::WavData& audio, bool attenuate, double& appliedGain, std::string& error) {
    return writePcm(path,audio,16,attenuate,"none",1,appliedGain,error);
}

bool writePcm(const std::filesystem::path& path,const runtime::WavData& audio,std::uint16_t bits,
    bool attenuate,const std::string& dither,std::uint32_t seed,double& appliedGain,std::string& error) {
    if((bits!=16 && bits!=24) || (dither!="none" && dither!="tpdf")) {error="PCM export supports PCM16/24 and explicit none/tpdf dither";return false;}
    const auto sampleBytes=bits/8;const auto scale=std::uint64_t{1}<<(bits-1);
    const auto maximum=static_cast<std::int64_t>(scale-1),minimum=-static_cast<std::int64_t>(scale);
    double peak = 0;
    if (!audio.channels || audio.channels>65535/sampleBytes || !audio.sampleRate ||
        static_cast<std::uint64_t>(audio.sampleRate)*audio.channels*sampleBytes>UINT32_MAX ||
        audio.interleaved.size() % audio.channels || audio.interleaved.size() > (0xffffffffull - 37) / sampleBytes) {
        error = "invalid or oversized PCM audio"; return false;
    }
    for (const auto value : audio.interleaved) {
        if (!std::isfinite(value)) { error = "non-finite PCM sample"; return false; }
        peak = std::max(peak, std::fabs(static_cast<double>(value)));
    }
    const double ceiling=static_cast<double>(maximum-(dither=="tpdf"?1:0))/scale;
    if(dither=="tpdf" && peak>ceiling && !attenuate) {error="TPDF dither requires quantizer headroom; choose explicit attenuation";return false;}
    if (peak > 1 && !attenuate) { error = "PCM overflow: choose explicit attenuation"; return false; }
    appliedGain = peak > ceiling && attenuate ? ceiling / peak : 1;
    auto temporary = path; temporary += ".pcm-tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot open PCM output"; return false; }
    const auto bytes = static_cast<std::uint32_t>(audio.interleaved.size() * sampleBytes);
    out.write("RIFF", 4); runtime::writeLe32(out, 36 + bytes+(bytes&1)); out.write("WAVEfmt ", 8);
    runtime::writeLe32(out, 16); runtime::writeLe16(out, 1); runtime::writeLe16(out, static_cast<std::uint16_t>(audio.channels));
    runtime::writeLe32(out, audio.sampleRate); runtime::writeLe32(out, audio.sampleRate * audio.channels * sampleBytes);
    runtime::writeLe16(out, static_cast<std::uint16_t>(audio.channels * sampleBytes)); runtime::writeLe16(out, bits);
    out.write("data", 4); runtime::writeLe32(out, bytes);
    std::uint32_t random=seed?seed:0x9e3779b9u;
    const auto uniform=[&]() {random^=random<<13;random^=random>>17;random^=random<<5;return static_cast<double>(random)/4294967296.0;};
    for (const auto value : audio.interleaved) {
        double noise=0;
        if(dither=="tpdf") {const auto first=uniform();const auto second=uniform();noise=first-second;}
        const auto quantized=std::clamp(static_cast<std::int64_t>(std::llround(value*appliedGain*scale+noise)),minimum,maximum);
        const auto code=static_cast<std::uint32_t>(quantized);
        for(int byte=0;byte<sampleBytes;++byte) out.put(static_cast<char>((code>>(byte*8))&255));
    }
    if(bytes&1) out.put(0);
    out.flush(); const bool ok = static_cast<bool>(out); out.close();
    if (!ok) { error = "failed to write PCM"; std::error_code ec; std::filesystem::remove(temporary, ec); return false; }
    return replace(temporary, path, error);
}

bool writeJsonAtomic(const std::filesystem::path& path, const persist::Json& value, std::string& error) {
    auto temporary = path; temporary += ".json-tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    const auto text = value.dump(); out.write(text.data(), static_cast<std::streamsize>(text.size())); out.flush();
    const bool ok = static_cast<bool>(out); out.close();
    if (!ok) { error = "failed to write JSON record"; std::error_code ec; std::filesystem::remove(temporary, ec); return false; }
    return replace(temporary, path, error);
}

persist::Json auditionRecords(const std::filesystem::path& audio, const std::filesystem::path& records, std::string& error) {
    auto all = readRecords(records, error); if (!error.empty()) return persist::Json::null();
    const auto hash = hashFile(audio);
    if (hash.empty()) { error = "audio is missing or unreadable"; return persist::Json::null(); }
    auto matching = persist::Json::array(); std::string status = "unheard";
    for (const auto& record : all.asArray()) {
        if (!record.find("fileHash") || record.find("fileHash")->asString() != hash) continue;
        matching.push(record);
        if (record.find("status") && record.find("status")->asString() == "heard") status = "heard";
    }
    auto report = persist::Json::object(); report.set("auditionStatus", persist::Json::string(status));
    report.set("fileHash", persist::Json::string(hash)); report.set("records", std::move(matching)); return report;
}

bool recordAudition(const std::filesystem::path& audio, const std::filesystem::path& records,
    const persist::Json& manifest, const std::string& status, const std::string& reviewer, std::string& error) {
    const auto* renderId = manifest.find("renderId");
    const auto hash = hashFile(audio);
    if (!renderId || renderId->asString().empty() || hash.empty() || reviewer.empty() || (status != "heard" && status != "playbackStarted")) {
        error = "audition requires a readable audio file, renderId, reviewer and valid status"; return false;
    }
    const auto* expected = manifest.find("fileHash");
    const auto* previewHash = manifest.find("previewHash");
    if ((!expected || expected->asString() != hash) && (!previewHash || previewHash->asString() != hash)) {
        error = "audio hash does not match render manifest"; return false;
    }
    auto all = readRecords(records, error); if (!error.empty()) return false;
    auto record = persist::Json::object(); record.set("renderId", *renderId); record.set("fileHash", persist::Json::string(hash));
    record.set("status", persist::Json::string(status)); record.set("reviewer", persist::Json::string(reviewer));
    record.set("approved", persist::Json::boolean(false));
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp; timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    record.set("timestamp", persist::Json::string(timestamp.str()));
    for (const auto* key : {"originSample", "frames", "previewStartTick", "previewEndTick"})
        if (const auto* value = manifest.find(key)) record.set(key, *value);
    all.push(std::move(record)); return writeJsonAtomic(records, all, error);
}

bool startPlayback(const std::filesystem::path& audio, std::string& error) {
    if (hashFile(audio).empty()) { error = "audio is missing or unreadable"; return false; }
#if defined(_WIN32)
    const auto file = std::filesystem::absolute(audio);
    const auto result = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, file.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) > 32) return true;
    error = "default player failed to start (ShellExecute code " + std::to_string(reinterpret_cast<std::intptr_t>(result)) + ")";
#else
    error = "playback is currently supported on Windows only";
#endif
    return false;
}
} // namespace nodsynth::song
