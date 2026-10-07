#include <nodsynth/song/ModelClient.h>

#include <fstream>
#include <atomic>
#include <chrono>
#include <set>
#include <algorithm>
#include <nodsynth/song/Workflow.h>
#include <nodsynth/song/Presets.h>
#include <nodsynth/song/Automation.h>

#include <nodsynth/song/Process.h>

namespace nodsynth::song {
namespace {
bool writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}
} // namespace

ModelProposal proposeEdits(const SongDocument& song, std::string_view instruction, const ModelAdapter& adapter)
{
    ModelProposal proposal;
    if (adapter.executable.empty()) {
        proposal.code = "missing-adapter";
        proposal.message = "model adapter was not configured";
        return proposal;
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto parent = std::filesystem::temp_directory_path() / "nodsynth-model";
    std::error_code failure;
    std::filesystem::create_directories(parent, failure);
    if (failure) {
        proposal.code = "output-io";
        proposal.message = "failed to create the model request directory";
        return proposal;
    }
    std::filesystem::path root;
    for (unsigned attempt=0;attempt<100;++attempt) {
        root=parent/(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
        if (std::filesystem::create_directory(root,failure)) break;
        root.clear();
    }
    if (root.empty()) {proposal.code="output-io";proposal.message="failed to isolate model request";return proposal;}
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {std::error_code ignored;std::filesystem::remove_all(path,ignored);}
    } cleanup{root};
    const auto queryPath = root / "query.json";
    const auto instructionPath = root / "instruction.txt";
    const auto outputPath = root / "commands.json";
    std::filesystem::remove(outputPath, failure);
    auto context=querySong(song);
    QueryOptions capabilities;capabilities.view="capabilities";
    context.set("capabilities",*querySong(song,capabilities).find("capabilities"));
    context.set("workflow",workflowJson(song,{}));
    context.set("presets",listJson(listPresets(defaultPresetsRoot())));
    context.set("proposalPolicy",persist::Json::string("Return schemaVersion 1 commands only; use stable target IDs and declared domains. Invalid commands are rejected in a dry run. Do not claim heard or approved."));
    if (!writeText(queryPath, context.dump()) || !writeText(instructionPath, instruction)) {
        proposal.code = "output-io";
        proposal.message = "failed to write the model request";
        return proposal;
    }
    ProcessRequest request;
    request.timeoutMs = adapter.timeoutMs;
    request.arguments.push_back(adapter.executable.string());
    request.arguments.insert(request.arguments.end(), adapter.arguments.begin(), adapter.arguments.end());
    request.arguments.push_back(queryPath.string());
    request.arguments.push_back(instructionPath.string());
    request.arguments.push_back(outputPath.string());
    const auto process = runProcess(request);
    if (!process.started) {
        proposal.code = "missing-adapter";
        proposal.message = process.message.empty() ? "failed to start the model adapter" : process.message;
        return proposal;
    }
    if (process.timedOut) {
        proposal.code = "external-timeout";
        proposal.message = process.message.empty() ? "the model adapter timed out" : process.message;
        return proposal;
    }
    if (process.exitCode != 0) {
        proposal.code = "model-failed";
        proposal.message = process.message.empty() ? "the model adapter failed" : process.message;
        return proposal;
    }
    std::ifstream input(outputPath, std::ios::binary);
    if (!input) {
        proposal.code = "model-failed";
        proposal.message = "the model adapter did not write a command batch";
        return proposal;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::string error;
    auto batch = persist::Json::parse(text, error);
    if (!batch || batch->kind() != persist::Json::Kind::object) {
        proposal.code = "model-failed";
        proposal.message = error.empty() ? "the model adapter wrote invalid commands" : error;
        return proposal;
    }
    auto copy=song;
    SongDocument staged;
    const auto checked=applyCommands(copy,*batch,song.revision,true,&staged);
    if (!checked.ok) {
        proposal.code=checked.code;proposal.message="invalid model proposal: "+checked.message;return proposal;
    }
    std::set<std::string> changedInstruments;
    const auto* commands=batch->find("commands");
    const auto emptyCommands=persist::Json::array();
    for (const auto& command:commands?commands->asArray():emptyCommands.asArray()) {
        if (!command.find("op")) continue;
        const auto op=command.find("op")->asString();const auto* track=command.find("track");
        if (track && (op=="set-parameter" || op=="set-parameter-automation" || op=="bind-preset" || op=="set-instrument" ||
            op=="set-vst3-instrument" || op=="set-sampler" || op=="bind-sample-kit")) changedInstruments.insert(track->asString());
    }
    for (const auto& track:staged.tracks) {
        if (!changedInstruments.contains(track.id) || (track.parameterValues.empty() && track.parameterAutomation.empty())) continue;
        const auto instrument=std::find_if(staged.instruments.begin(),staged.instruments.end(),[&](const auto& i){return i.id==track.instrumentId;});
        if (instrument==staged.instruments.end()) {
            proposal.code="missing-instrument";proposal.message="parameter proposal requires a bound instrument: "+track.id;return proposal;
        }
        if (instrument->kind==InstrumentKind::vst3) {
            auto pending=persist::Json::object();pending.set("track",persist::Json::string(track.id));
            pending.set("check",persist::Json::string("isolated-plugin-parameter-inspection"));
            proposal.pendingChecks.push(std::move(pending));continue;
        }
        if (instrument->kind!=InstrumentKind::nodsynth) {
            proposal.code="unsupported-backend";proposal.message="parameter proposal backend does not support lanes: "+track.id;return proposal;
        }
        const auto resource=std::find_if(staged.resources.begin(),staged.resources.end(),[&](const auto& r){return r.id==instrument->resourceId;});
        if (resource==staged.resources.end()) {proposal.code="missing-resource";proposal.message="parameter proposal has no patch resource";return proposal;}
        const auto graph=render::loadPatch(resolveResourcePath(staged.baseDirectory,resource->path),error);
        if (!graph) {proposal.code="missing-patch";proposal.message=error;return proposal;}
        std::vector<PreparedLane> lanes;
        if (!prepareAutomation(staged,track,*graph,nullptr,48000,lanes,error)) {
            proposal.code=automationErrorCode(error);proposal.message="invalid parameter proposal for "+track.id+": "+error;return proposal;
        }
    }
    batch->set("baseRevision",persist::Json::number(static_cast<double>(song.revision)));
    proposal.diff=checked.diff;
    proposal.ok = true;
    proposal.message = "proposed";
    proposal.batch = std::move(*batch);
    return proposal;
}
} // namespace nodsynth::song
