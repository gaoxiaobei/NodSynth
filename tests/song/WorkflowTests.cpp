#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nodsynth/song/Workflow.h>
#include <nodsynth/song/Presets.h>
#include <nodsynth/song/ModelClient.h>
#include <nodsynth/persist/ProjectFile.h>

#include <chrono>
#include <cmath>
#include <future>

using namespace nodsynth;
namespace {
persist::Json json(const std::string& text) {
    std::string error;auto result=persist::Json::parse(text,error);REQUIRE(result);return *result;
}
song::SongDocument source() {
    auto document=song::createSong({120,4,4,480,4});
    song::Track track;track.id="melody";track.name="Melody";track.role="lead";track.gainMode="multiply";
    song::Clip clip;clip.id="phrase";clip.startTick=400;clip.length=800;
    clip.notes={{"held",0,400,60,100,0},{"ending",600,200,64,80,0}};
    track.clips.push_back(clip);track.gainAutomation={{0,.5},{900,.8}};
    document.tracks.push_back(track);return document;
}
song::ApplyResult apply(song::SongDocument& document,const std::string& commands,bool dry=false) {
    return song::applyCommands(document,json("{\"schemaVersion\":1,\"commands\":"+commands+"}"),document.revision,dry);
}
}

TEST_CASE("Workflow roles are explicit or preset-derived and starting chains are reviewable", "[workflow]") {
    auto document=source();song::QueryOptions options;options.view="roles";options.role="lead";
    auto report=song::querySong(document,options);REQUIRE(report.find("tracks")->asArray().size()==1);
    const auto proposal=*report.find("tracks")->asArray()[0].find("startingChainProposal");
    const auto before=song::toJson(document).dump();
    REQUIRE(song::applyCommands(document,proposal,document.revision,true).ok);
    REQUIRE(song::toJson(document).dump()==before);
    REQUIRE(apply(document,R"([{"op":"set-role","track":"melody","role":"pad"}])").ok);
    REQUIRE_FALSE(song::applyCommands(document,proposal).ok);
    REQUIRE(song::querySong(document,options).find("tracks")->asArray().empty());
    REQUIRE(song::undoSong(document).ok);REQUIRE(document.tracks[0].role=="lead");
    document.tracks[0].role.clear();options.role.reset();
    REQUIRE(song::querySong(document,options).find("tracks")->asArray()[0].find("startingChainProposal")->isNull());
}

TEST_CASE("Workflow sections survive persistence and invalid structure edits are atomic", "[workflow]") {
    auto document=source();
    const auto edited=apply(document,R"([{"op":"set-section","id":"verse","name":"Verse","startTick":500,"endTick":1100}])");
    REQUIRE(edited.ok);REQUIRE(edited.diff.find("changedEntityCount")->asNumber()>0);
    std::string error;auto loaded=song::songFromJson(song::toJson(document),error);REQUIRE(loaded);
    REQUIRE(loaded->sections[0].name=="Verse");REQUIRE(loaded->tracks[0].role=="lead");
    const auto before=song::toJson(document).dump();
    REQUIRE_FALSE(apply(document,R"([{"op":"set-role","track":"melody","role":"pad"},{"op":"set-section","id":"bad","startTick":100,"endTick":100}])").ok);
    REQUIRE(song::toJson(document).dump()==before);
    REQUIRE(apply(document,R"([{"op":"delete-section","id":"verse"}])").ok);
    REQUIRE(document.sections.empty());REQUIRE(song::undoSong(document).ok);REQUIRE(document.sections.size()==1);
}

TEST_CASE("Repeat phrase clips boundary notes preserves controls and rejects collisions", "[workflow]") {
    auto document=source();const auto controls=document.tracks[0].gainAutomation;
    REQUIRE(apply(document,R"([{"op":"repeat-phrase","id":"copy","track":"melody","startTick":500,"endTick":1100,"destinationTick":1920}])").ok);
    const auto& copy=document.tracks[0].clips.back();REQUIRE(copy.startTick==1920);REQUIRE(copy.length==600);
    REQUIRE(copy.notes.size()==2);REQUIRE(copy.notes[0].tick==0);REQUIRE(copy.notes[0].duration==300);
    REQUIRE(copy.notes[1].tick==500);REQUIRE(copy.notes[1].duration==100);
    REQUIRE(document.tracks[0].gainAutomation.size()==controls.size());
    REQUIRE(document.tracks[0].gainAutomation[1].gain==controls[1].gain);
    const auto before=song::toJson(document).dump();
    REQUIRE_FALSE(apply(document,R"([{"op":"repeat-phrase","id":"copy","startTick":500,"endTick":1100,"destinationTick":3840}])").ok);
    REQUIRE(song::toJson(document).dump()==before);
    REQUIRE_FALSE(apply(document,R"([{"op":"repeat-phrase","id":"overflow","startTick":500,"endTick":1100,"destinationTick":4294967295}])").ok);
    REQUIRE_FALSE(apply(document,R"([{"op":"repeat-phrase","id":"outside","startTick":500,"endTick":1100,"destinationTick":7680}])").ok);
    REQUIRE(apply(document,R"([{"op":"repeat-phrase","id":"outside","startTick":500,"endTick":1100,"destinationTick":7680,"extendSongRange":true}])").ok);
    REQUIRE(*document.songRangeEndTick==8280);
}

TEST_CASE("Preset search requires every case-insensitive term and respects role", "[workflow]") {
    const auto results=song::searchPresets(NOD_PRESETS_ROOT,"PRODUCTION stereo","lead");
    REQUIRE(results.size()==1);REQUIRE(results[0].id=="production-lead");
    REQUIRE(song::searchPresets(NOD_PRESETS_ROOT,"never-matches").empty());
}

TEST_CASE("Workflow metrics count actual trace attempts and require frozen resource identity", "[workflow]") {
    auto trace=json(R"({"actor":"agent","resourceSnapshotHash":"frozen-fixture","events":[
        {"kind":"command-batch","intentId":"tone","commands":[{"op":"set-parameter-automation"}],"outcome":"rejected","code":"control-override"},
        {"kind":"source-lookup"},{"kind":"human-intervention"},
        {"kind":"tool-call","tool":"nod","arguments":["song","query"],"exitCode":0},
        {"kind":"command-batch","intentId":"tone","commands":[{"op":"set-role"},{"op":"set-inserts"}],"outcome":"ok"}]})");
    const auto report=song::workflowMetrics(trace);REQUIRE(report.find("status")->asString()=="ok");
    REQUIRE(report.find("commands")->asNumber()==3);REQUIRE(report.find("retries")->asNumber()==1);
    REQUIRE(report.find("toolCalls")->asNumber()==1);
    REQUIRE(report.find("sourceLookups")->asNumber()==1);REQUIRE(report.find("humanInterventions")->asNumber()==1);
    REQUIRE(report.find("invalidLaneAttempts")->asNumber()==1);
    trace.set("resourceSnapshotHash",persist::Json::string(""));
    REQUIRE(song::workflowMetrics(trace).find("status")->asString()=="rejected");
}

TEST_CASE("Model workflow rejects invalid proposals and isolates concurrent adapter requests", "[workflow]") {
    const auto document=source();song::ModelAdapter adapter;adapter.executable=NOD_MODEL_ADAPTER;
    const auto before=song::toJson(document).dump();
    REQUIRE_FALSE(song::proposeEdits(document,"invalid-workflow-proposal",adapter).ok);
    auto first=std::async(std::launch::async,[&]{return song::proposeEdits(document,"raise melody",adapter);});
    auto second=std::async(std::launch::async,[&]{return song::proposeEdits(document,"raise melody",adapter);});
    const auto a=first.get(),b=second.get();REQUIRE(a.ok);REQUIRE(b.ok);
    REQUIRE(a.batch.dump()==b.batch.dump());REQUIRE(a.batch.find("baseRevision")->asNumber()==document.revision);
    REQUIRE(song::toJson(document).dump()==before);
}

TEST_CASE("Version audition binds sources and matches loudness without overwriting", "[workflow]") {
    const auto root=std::filesystem::temp_directory_path()/("nod-workflow-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);std::string error;
    auto document=source();document.baseDirectory=root;
    REQUIRE(song::bindPreset(document,"melody","production-lead",NOD_PRESETS_ROOT,error));
    document.sections.push_back({"phrase-section","Phrase",0,1920});
    const auto a=root/"a.json",b=root/"b.json";
    REQUIRE(song::saveSong(a,document,error));document.tracks[0].gain=.25;REQUIRE(song::saveSong(b,document,error));
    const auto hashA=song::hashFile(a),hashB=song::hashFile(b);
    song::SongRenderOptions options;options.tailSeconds=1;options.useCache=false;
    const auto report=song::renderVersions(a,b,root/"audition",options,{},"phrase-section");
    INFO(report.dump());REQUIRE(report.find("status")->asString()=="ok");
    const auto& versions=report.find("versions")->asArray();REQUIRE(versions.size()==2);
    const auto loudness=[](const auto& entry){return entry.find("analysis")->find("loudnessLufs")->asNumber();};
    REQUIRE(std::abs(loudness(versions[0])-loudness(versions[1]))<.05);
    REQUIRE(versions[0].find("auditionStatus")->asString()=="unheard");
    REQUIRE(song::hashFile(a)==hashA);REQUIRE(song::hashFile(b)==hashB);
    REQUIRE(song::renderVersions(a,b,root/"audition",options).find("status")->asString()=="rejected");
    std::filesystem::remove_all(root);
}

TEST_CASE("Model proposals validate native parameter addresses before export without collecting assets", "[workflow]") {
    const auto root=std::filesystem::temp_directory_path()/("nod-proposal-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);std::string error;
    auto document=source();document.baseDirectory=root;
    REQUIRE(song::bindPreset(document,"melody","production-lead",NOD_PRESETS_ROOT,error));
    song::ModelAdapter adapter;adapter.executable=NOD_MODEL_ADAPTER;
    const auto before=song::toJson(document).dump();
    const auto unknown=song::proposeEdits(document,"parameter-workflow:absent/cutoff",adapter);
    REQUIRE_FALSE(unknown.ok);REQUIRE(unknown.code=="unknown-address");
    const auto unsupported=song::proposeEdits(document,"parameter-workflow:oscillator/voices",adapter);
    REQUIRE_FALSE(unsupported.ok);REQUIRE(unsupported.code=="unsupported-target");
    const auto valid=song::proposeEdits(document,"parameter-workflow:filter/cutoff",adapter);
    INFO(valid.message);REQUIRE(valid.ok);REQUIRE(valid.pendingChecks.asArray().empty());
    REQUIRE(song::toJson(document).dump()==before);REQUIRE_FALSE(std::filesystem::exists(root/"assets"));

    auto patch=persist::loadProject(std::filesystem::path(NOD_PRESETS_ROOT)/"production-roles/production-lead.json",error);REQUIRE(patch);
    patch->graph.macros.clear();
    for (auto& node:patch->graph.nodes) if (node.id.value=="filter") {node.typeId.value="nod.lowpass";node.schemaVersion=1;}
    for (auto& wire:patch->graph.connections) if (wire.to.nodeId.value=="filter" && wire.to.portId.value=="cutoff-mod") wire.to.portId.value="cutoff";
    const auto path=root/"legacy-control.json";REQUIRE(persist::saveProject(path,*patch,error));
    REQUIRE(song::bindPatch(document,"melody",path.string(),path,error));
    const auto blocked=song::proposeEdits(document,"parameter-workflow:filter/cutoff",adapter);
    INFO(blocked.message);REQUIRE_FALSE(blocked.ok);REQUIRE(blocked.code=="control-override");
    std::filesystem::remove_all(root);
}
