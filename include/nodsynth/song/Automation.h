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
    bool automatable{true};
    std::string reason;
    std::vector<model::Connection> blockingConnections;
};
struct PreparedLane {
    std::uint32_t index{0};
    double base{0};
    bool linear{true};
    bool logarithmic{false};
    struct Point { std::int64_t sample; double value; };
    std::vector<Point> points;
};
[[nodiscard]] std::vector<ParameterTarget> parameterTargets(const model::GraphSnapshot& graph);
[[nodiscard]] persist::Json parametersJson(const model::GraphSnapshot& graph);
[[nodiscard]] bool prepareAutomation(const SongDocument& song, const Track& track,
    const model::GraphSnapshot& graph, const runtime::RuntimePlan* plan, std::uint32_t rate,
    std::vector<PreparedLane>& lanes, std::string& error);
[[nodiscard]] double parameterAt(const PreparedLane& lane, std::int64_t sample);
[[nodiscard]] std::string automationErrorCode(const std::string& error);
[[nodiscard]] persist::Json mixControlsJson(const Track& track);
[[nodiscard]] persist::Json productionMigrationPreview(const model::GraphSnapshot& graph);
[[nodiscard]] bool validateMacros(const model::GraphSnapshot& graph,std::string& error);
[[nodiscard]] std::optional<model::GraphSnapshot> migrateProductionGraph(const model::GraphSnapshot& graph,const persist::Json& decisions,std::string& error);

struct PreparedTrackMix {
    PreparedLane gain;
    double fader{1}, pan{0};
    double panLeft{0.7071067811865476}, panRight{0.7071067811865475};
    bool multiply{false}, balance{false};
    struct Interval { std::int64_t start, end; };
    struct Dip { std::int64_t start, recovery, end; };
    std::vector<Dip> dips;
    std::vector<Interval> skip, mute;
    double depth{0};
    std::int64_t pumpFade{0}, muteFade{0};
    [[nodiscard]] double gainAt(std::int64_t sample) const;
    void gainsAt(std::int64_t sample, float& left, float& right) const;
};
[[nodiscard]] bool prepareTrackMix(const SongDocument& song, const Track& track, std::uint32_t rate,
    PreparedTrackMix& mix, std::string& error);
} // namespace nodsynth::song
