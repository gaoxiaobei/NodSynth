#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nodsynth/persist/Json.h>
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::song {
struct ModelAdapter {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::uint32_t timeoutMs{30000};
};

struct ModelProposal {
    bool ok{false};
    std::string code;
    std::string message;
    persist::Json batch{persist::Json::object()};
    persist::Json diff{persist::Json::object()};
    persist::Json pendingChecks{persist::Json::array()};
};

// Ask an external model adapter for a command batch. The adapter receives the song query and the
// instruction as files and writes a command batch. This does not modify the song.
[[nodiscard]] ModelProposal proposeEdits(const SongDocument& song, std::string_view instruction, const ModelAdapter& adapter);
} // namespace nodsynth::song
