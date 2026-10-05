#pragma once
#include <nodsynth/song/SongDocument.h>

namespace nodsynth::song {
[[nodiscard]] bool applyPhrase(SongDocument& song, const persist::Json& command, persist::Json& idMap, std::string& error);
} // namespace nodsynth::song
