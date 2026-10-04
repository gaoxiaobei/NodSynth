#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/persist/Json.h>

namespace nodsynth::persist {
inline constexpr int kProjectFormatVersion = 1;

struct ProjectDocument {
    Json root{Json::object()};
    model::Viewport viewport{};
    model::GraphSnapshot graph{};
};

[[nodiscard]] std::optional<ProjectDocument> loadProject(const std::filesystem::path& path, std::string& error);
[[nodiscard]] bool saveProject(const std::filesystem::path& path, ProjectDocument& document, std::string& error);
[[nodiscard]] ProjectDocument projectFromGraph(model::GraphSnapshot graph, model::Viewport viewport = {});
} // namespace nodsynth::persist
