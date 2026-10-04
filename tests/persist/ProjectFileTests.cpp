#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>

using namespace nodsynth::persist;

TEST_CASE("projects round-trip nodes, positions, parameters, and unknown data") {
    auto document = projectFromGraph(nodsynth::nodes::sinePatch(), {12.f, -3.f, 1.25f});
    document.graph.nodes.front().extensionsJson = R"({"vendor":7})";
    document.root.set("comment", Json::string("keep-me"));
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-roundtrip.nodsynth.json";
    std::string error;
    REQUIRE(saveProject(path, document, error));
    const auto loaded = loadProject(path, error);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->viewport.zoom == 1.25f);
    REQUIRE(loaded->graph.nodes.size() == document.graph.nodes.size());
    REQUIRE(loaded->graph.connections.size() == document.graph.connections.size());
    REQUIRE(loaded->graph.nodes[1].position.x == document.graph.nodes[1].position.x);
    REQUIRE(loaded->root.find("comment")->asString() == "keep-me");
    std::string extraError;
    const auto extras = Json::parse(loaded->graph.nodes.front().extensionsJson, extraError);
    REQUIRE(extras.has_value());
    REQUIRE(extras->find("vendor")->asNumber() == 7.0);
    std::filesystem::remove(path);
}

TEST_CASE("corrupt and unsupported projects are rejected") {
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-corrupt.nodsynth.json";
    {
        std::ofstream out(path);
        out << "{not json";
    }
    std::string error;
    REQUIRE_FALSE(loadProject(path, error).has_value());
    {
        std::ofstream out(path);
        out << R"({"format":"nodsynth.project","formatVersion":99,"nodes":[],"connections":[]})";
    }
    REQUIRE_FALSE(loadProject(path, error).has_value());
    REQUIRE(error.find("not supported") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("a failed save leaves the previous project intact") {
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-atomic.nodsynth.json";
    auto document = projectFromGraph(nodsynth::nodes::sinePatch());
    std::string error;
    REQUIRE(saveProject(path, document, error));
    const auto directory = std::filesystem::temp_directory_path() / "nodsynth-not-a-file";
    std::filesystem::create_directory(directory);
    ProjectDocument blocked = document;
    blocked.graph.nodes.clear();
    REQUIRE_FALSE(saveProject(directory, blocked, error));
    const auto reloaded = loadProject(path, error);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->graph.nodes.size() == document.graph.nodes.size());
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

#if defined(_WIN32)
TEST_CASE("a failed overwrite leaves the previous project intact") {
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-readonly.nodsynth.json";
    auto document = projectFromGraph(nodsynth::nodes::sinePatch());
    std::string error;
    REQUIRE(saveProject(path, document, error));
    const auto original = loadProject(path, error);
    REQUIRE(original.has_value());
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    auto replacement = document;
    replacement.graph.nodes.clear();
    const bool saved = saveProject(path, replacement, error);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    REQUIRE_FALSE(saved);
    const auto reloaded = loadProject(path, error);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->graph.nodes.size() == original->graph.nodes.size());
    std::filesystem::remove(path);
}
#endif
