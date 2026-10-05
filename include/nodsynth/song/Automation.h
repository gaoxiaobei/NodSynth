#pragma once

#include <nodsynth/song/SongDocument.h>
#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/runtime/RuntimePlan.h>

namespace nodsynth::song {
struct ParameterTarget {
    std::string id;
    model::NodeId node;
    model::ParameterId parameter;
    std::string unit;
    double minimum{0}, maximum{1}, base{0};
    bool sourceLevel{false};
};
struct PreparedLane {
    std::uint32_t index{0};
    double base{0};
    bool linear{true};
    struct Point { std::int64_t sample; double value; };
    std::vector<Point> points;
};
[[nodiscard]] std::vector<ParameterTarget> parameterTargets(const model::GraphSnapshot& graph);
[[nodiscard]] persist::Json parametersJson(const model::GraphSnapshot& graph);
[[nodiscard]] bool prepareAutomation(const SongDocument& song, const Track& track,
    const model::GraphSnapshot& graph, const runtime::RuntimePlan* plan, std::uint32_t rate,
    std::vector<PreparedLane>& lanes, std::string& error);
[[nodiscard]] double parameterAt(const PreparedLane& lane, std::int64_t sample);
} // namespace nodsynth::song
