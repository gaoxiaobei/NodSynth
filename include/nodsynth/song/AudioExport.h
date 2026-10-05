#pragma once
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/persist/Json.h>

namespace nodsynth::song {
[[nodiscard]] bool writePcm16(const std::filesystem::path& path, const runtime::WavData& audio,
    bool attenuate, double& appliedGain, std::string& error);
[[nodiscard]] bool writeJsonAtomic(const std::filesystem::path& path, const persist::Json& value, std::string& error);
[[nodiscard]] persist::Json auditionRecords(const std::filesystem::path& audio, const std::filesystem::path& records, std::string& error);
[[nodiscard]] bool recordAudition(const std::filesystem::path& audio, const std::filesystem::path& records,
    const persist::Json& manifest, const std::string& status, const std::string& reviewer, std::string& error);
[[nodiscard]] bool startPlayback(const std::filesystem::path& audio, std::string& error);
} // namespace nodsynth::song
