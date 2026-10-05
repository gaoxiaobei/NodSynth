#include <nodsynth/song/ModelClient.h>

#include <fstream>

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
    const auto root = std::filesystem::temp_directory_path() / "nodsynth-model";
    std::error_code failure;
    std::filesystem::create_directories(root, failure);
    if (failure) {
        proposal.code = "output-io";
        proposal.message = "failed to create the model request directory";
        return proposal;
    }
    const auto queryPath = root / "query.json";
    const auto instructionPath = root / "instruction.txt";
    const auto outputPath = root / "commands.json";
    std::filesystem::remove(outputPath, failure);
    if (!writeText(queryPath, querySong(song).dump()) || !writeText(instructionPath, instruction)) {
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
    proposal.ok = true;
    proposal.message = "proposed";
    proposal.batch = std::move(*batch);
    return proposal;
}
} // namespace nodsynth::song
