#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nodsynth/persist/Json.h>
#include <nodsynth/render/OfflineRenderer.h>
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::song {
struct PresetInfo {
    std::string id;
    int version{1};
    std::string role;
    std::string name;
    std::vector<std::string> tags;
    std::string patchPath;
    std::string hash;
};

struct PresetAuditionOptions {
    std::filesystem::path presetsRoot;
    double sampleRate{48000.0};
    std::uint32_t blockSize{128};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};
    double noteSeconds{0.12};
    double tailSeconds{0.35};
};

[[nodiscard]] std::filesystem::path defaultPresetsRoot();
[[nodiscard]] std::vector<PresetInfo> listPresets(
    const std::filesystem::path& presetsRoot, std::optional<std::string> role = std::nullopt);
[[nodiscard]] std::optional<PresetInfo> findPreset(const std::filesystem::path& presetsRoot, std::string_view id);
[[nodiscard]] persist::Json presetJson(const PresetInfo& info);
[[nodiscard]] persist::Json listJson(const std::vector<PresetInfo>& presets);
[[nodiscard]] bool bindPreset(
    SongDocument& song,
    const std::string& trackId,
    std::string_view presetId,
    const std::filesystem::path& presetsRoot,
    std::string& error);
[[nodiscard]] render::RenderReport renderPresetAudition(
    std::string_view presetId, const std::filesystem::path& outputWav, const PresetAuditionOptions& options);
} // namespace nodsynth::song
