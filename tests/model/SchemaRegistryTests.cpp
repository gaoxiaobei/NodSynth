#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/SchemaRegistry.h>

using namespace nodsynth::model;

static NodeSchema oscillatorSchema() {
    return {
        .typeId = NodeTypeId{"org.nodsynth.oscillator.analog"},
        .schemaVersion = 1,
        .displayName = "Oscillator",
        .category = "Sources/Oscillators",
        .sourceId = "org.nodsynth.builtin",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"frequency"}, "Frequency", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode},
            {PortId{"audio"}, "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

TEST_CASE("schema registry finds a registered stable type ID") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(oscillatorSchema()));
    REQUIRE(registry.find(NodeTypeId{"org.nodsynth.oscillator.analog"}) != nullptr);
}

TEST_CASE("schema registry rejects duplicate type IDs") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(oscillatorSchema()));
    REQUIRE_FALSE(registry.registerSchema(oscillatorSchema()));
    REQUIRE(registry.size() == 1);
}
