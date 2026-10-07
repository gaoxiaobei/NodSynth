#pragma once

#include <nodsynth/runtime/DspNode.h>

namespace nodsynth::nodes {
inline constexpr std::uint32_t kUnisonMaximum = 8;
inline constexpr std::uint32_t kSawTableSize = 4096;
inline constexpr std::uint32_t kSawTableLevels = 11;
[[nodiscard]] std::unique_ptr<runtime::DspNode> makeUnisonOscillator();
[[nodiscard]] std::uint64_t unisonStateBytes(std::uint32_t voices) noexcept;
}
