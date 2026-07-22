#pragma once
#include <string_view>

namespace nodsynth::model {
struct BuildInfo {
    int coreApiMajor;
    int coreApiMinor;
    std::string_view projectName;
};

[[nodiscard]] BuildInfo buildInfo() noexcept;
} // namespace nodsynth::model
