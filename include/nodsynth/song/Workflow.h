#pragma once

#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/SongRenderer.h>

namespace nodsynth::song {
[[nodiscard]] persist::Json workflowJson(const SongDocument& song, const QueryOptions& options);
[[nodiscard]] std::string trackRole(const SongDocument& song, const Track& track);
[[nodiscard]] persist::Json startingChain(const std::string& role);
[[nodiscard]] bool applyWorkflow(SongDocument& song, const persist::Json& command,
    persist::Json& idMap, std::string& error);
[[nodiscard]] persist::Json renderVersions(const std::filesystem::path& a, const std::filesystem::path& b,
    const std::filesystem::path& output, SongRenderOptions options, const std::string& bars = {},
    const std::string& section = {});
[[nodiscard]] persist::Json workflowMetrics(const persist::Json& trace);
} // namespace nodsynth::song
