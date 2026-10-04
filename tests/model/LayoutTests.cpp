#include <catch2/catch_test_macros.hpp>

#include <nodsynth/model/GraphEditor.h>

using namespace nodsynth::model;

TEST_CASE("node movement is undoable and does not change structure revision") {
    GraphEditor editor;
    REQUIRE(editor.addNode({NodeId{"osc"}, NodeTypeId{"nod.oscillator"}, 1, {}, "{}", {1.f, 2.f}}));
    const auto revision = editor.structureRevision();
    REQUIRE(editor.moveNode(NodeId{"osc"}, {30.f, 40.f}));
    REQUIRE(editor.recentChange() == ChangeKind::layout);
    REQUIRE(editor.structureRevision() == revision);
    REQUIRE(editor.document().findNode(NodeId{"osc"})->position.x == 30.f);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().findNode(NodeId{"osc"})->position.x == 1.f);
    REQUIRE(editor.structureRevision() == revision);
}

TEST_CASE("loading a snapshot clears undo history and advances the structure revision") {
    GraphEditor editor;
    REQUIRE(editor.addNode({NodeId{"a"}, NodeTypeId{"nod.gain"}, 1, {}, "{}", {}}));
    const auto before = editor.structureRevision();
    GraphSnapshot snapshot{{NodeRecord{NodeId{"b"}, NodeTypeId{"nod.oscillator"}, 1, {}, "{}", {8.f, 9.f}}}, {}};
    editor.load(std::move(snapshot));
    REQUIRE(editor.structureRevision() == before + 1);
    REQUIRE(editor.document().nodes().size() == 1);
    REQUIRE(editor.document().nodes().front().id.value == "b");
    REQUIRE_FALSE(editor.undo());
    REQUIRE(editor.setViewport({10.f, -4.f, 1.5f}));
    REQUIRE(editor.document().viewport().zoom == 1.5f);
    REQUIRE_FALSE(editor.setViewport({0.f, 0.f, 0.f}));
}
