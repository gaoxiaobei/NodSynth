#include <catch2/catch_test_macros.hpp>

#include <nodsynth/model/GraphEditor.h>

using namespace nodsynth::model;

TEST_CASE("remove node undo restores the node and its connections") {
    GraphEditor editor;
    editor.addNode({NodeId{"osc-1"}, NodeTypeId{"osc"}, 1,
                    {{ParameterId{"gain"}, 0.5}}, "{}", {10.0F, 20.0F}});
    editor.addNode({NodeId{"out-1"}, NodeTypeId{"out"}, 1, {}, "{}", {300.0F, 20.0F}});
    editor.connect({{NodeId{"osc-1"}, PortId{"audio"}}, {NodeId{"out-1"}, PortId{"audio"}}});

    REQUIRE(editor.removeNode(NodeId{"osc-1"}));
    REQUIRE(editor.document().nodes().size() == 1);
    REQUIRE(editor.document().connections().empty());

    REQUIRE(editor.undo());
    REQUIRE(editor.document().nodes().size() == 2);
    REQUIRE(editor.document().connections().size() == 1);
    REQUIRE(editor.redo());
    REQUIRE(editor.document().nodes().size() == 1);
}

TEST_CASE("an input accepts at most one connection") {
    GraphEditor editor;
    editor.addNode({NodeId{"a"}, NodeTypeId{"source"}, 1, {}, "{}", {}});
    editor.addNode({NodeId{"b"}, NodeTypeId{"source"}, 1, {}, "{}", {}});
    editor.addNode({NodeId{"c"}, NodeTypeId{"sink"}, 1, {}, "{}", {}});
    REQUIRE(editor.connect({{NodeId{"a"}, PortId{"out"}}, {NodeId{"c"}, PortId{"in"}}}));
    REQUIRE_FALSE(editor.connect({{NodeId{"b"}, PortId{"out"}}, {NodeId{"c"}, PortId{"in"}}}));
}

TEST_CASE("node IDs are unique and parameter edits undo") {
    GraphEditor editor;
    const NodeRecord node{NodeId{"node"}, NodeTypeId{"gain"}, 1,
                          {{ParameterId{"gain"}, 0.5}}, "{}", {}};
    REQUIRE(editor.addNode(node));
    REQUIRE_FALSE(editor.addNode(node));
    REQUIRE(editor.setParameter(NodeId{"node"}, ParameterId{"gain"}, 0.8));
    REQUIRE(editor.document().nodes().front().parameters.at(ParameterId{"gain"}) == 0.8);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().nodes().front().parameters.at(ParameterId{"gain"}) == 0.5);
}

TEST_CASE("remove node undo preserves original node and connection order") {
    GraphEditor editor;
    editor.addNode({NodeId{"first"}, NodeTypeId{"source"}, 1, {}, "{}", {}});
    editor.addNode({NodeId{"removed"}, NodeTypeId{"processor"}, 7,
                    {{ParameterId{"amount"}, 0.75}}, R"({"mode":"ring"})", {12.5F, -4.0F}});
    editor.addNode({NodeId{"last"}, NodeTypeId{"sink"}, 1, {}, "{}", {}});

    const Connection before{{NodeId{"first"}, PortId{"out"}}, {NodeId{"last"}, PortId{"in"}}};
    const Connection incidentOne{{NodeId{"first"}, PortId{"side"}}, {NodeId{"removed"}, PortId{"in"}}};
    const Connection incidentTwo{{NodeId{"removed"}, PortId{"out"}}, {NodeId{"last"}, PortId{"side"}}};
    REQUIRE(editor.connect(before));
    REQUIRE(editor.connect(incidentOne));
    REQUIRE(editor.connect(incidentTwo));
    const GraphSnapshot snapshot = editor.document().snapshot();

    REQUIRE(editor.removeNode(NodeId{"removed"}));
    REQUIRE(editor.undo());

    const GraphSnapshot restored = editor.document().snapshot();
    REQUIRE(restored.nodes.size() == snapshot.nodes.size());
    REQUIRE(restored.nodes[1].id == snapshot.nodes[1].id);
    REQUIRE(restored.nodes[1].typeId == snapshot.nodes[1].typeId);
    REQUIRE(restored.nodes[1].schemaVersion == snapshot.nodes[1].schemaVersion);
    REQUIRE(restored.nodes[1].parameters == snapshot.nodes[1].parameters);
    REQUIRE(restored.nodes[1].opaqueStateJson == snapshot.nodes[1].opaqueStateJson);
    REQUIRE(restored.nodes[1].position.x == snapshot.nodes[1].position.x);
    REQUIRE(restored.nodes[1].position.y == snapshot.nodes[1].position.y);
    REQUIRE(restored.connections == snapshot.connections);
}

TEST_CASE("disconnect can be undone at its original connection position") {
    GraphEditor editor;
    const Connection first{{NodeId{"a"}, PortId{"out"}}, {NodeId{"b"}, PortId{"first"}}};
    const Connection second{{NodeId{"a"}, PortId{"side"}}, {NodeId{"b"}, PortId{"second"}}};
    REQUIRE(editor.connect(first));
    REQUIRE(editor.connect(second));

    REQUIRE(editor.disconnect(first));
    REQUIRE(editor.undo());
    REQUIRE(editor.document().connections().front() == first);
}

TEST_CASE("a snapshot does not change with later edits") {
    GraphEditor editor;
    REQUIRE(editor.addNode({NodeId{"node"}, NodeTypeId{"gain"}, 1, {}, "{}", {}}));
    const GraphSnapshot snapshot = editor.document().snapshot();

    REQUIRE(editor.setParameter(NodeId{"node"}, ParameterId{"gain"}, 0.8));
    REQUIRE(snapshot.nodes.front().parameters.empty());
    REQUIRE(editor.document().nodes().front().parameters.at(ParameterId{"gain"}) == 0.8);
}
