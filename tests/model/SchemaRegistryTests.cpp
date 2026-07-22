#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/SchemaRegistry.h>

#include <limits>
#include <utility>

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

static NodeSchema schemaWithParameter(double minimum, double maximum, double defaultValue) {
    auto schema = oscillatorSchema();
    schema.typeId = NodeTypeId{"org.nodsynth.parameter-test"};
    schema.parameters = {{
        ParameterId{"amount"}, "Amount", "", minimum, maximum, defaultValue,
        ParameterScale::linear, true,
    }};
    return schema;
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

TEST_CASE("schema registry rejects output domains that differ from the owner scope") {
    SECTION("global node with per-voice output") {
        SchemaRegistry registry;
        auto schema = oscillatorSchema();
        schema.scope = NodeScope::global;
        schema.ports.back().domain = PortDomain::perVoice;
        REQUIRE_FALSE(registry.registerSchema(std::move(schema)));
    }

    SECTION("per-voice node with global output") {
        SchemaRegistry registry;
        auto schema = oscillatorSchema();
        schema.ports.back().domain = PortDomain::global;
        REQUIRE_FALSE(registry.registerSchema(std::move(schema)));
    }
}

TEST_CASE("schema registry only permits the explicit voice-mix input scope crossing") {
    SECTION("global owner accepts an explicitly per-voice input") {
        SchemaRegistry registry;
        auto schema = oscillatorSchema();
        schema.scope = NodeScope::global;
        schema.ports.front().domain = PortDomain::perVoice;
        REQUIRE(registry.registerSchema(std::move(schema)));
    }

    SECTION("per-voice owner rejects a global input") {
        SchemaRegistry registry;
        auto schema = oscillatorSchema();
        schema.ports.front().domain = PortDomain::global;
        REQUIRE_FALSE(registry.registerSchema(std::move(schema)));
    }
}

TEST_CASE("schema registry rejects NaN parameter bounds") {
    const auto nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("NaN minimum") {
        SchemaRegistry registry;
        REQUIRE_FALSE(registry.registerSchema(schemaWithParameter(nan, 1.0, 0.5)));
    }

    SECTION("NaN maximum") {
        SchemaRegistry registry;
        REQUIRE_FALSE(registry.registerSchema(schemaWithParameter(0.0, nan, 0.5)));
    }
}

TEST_CASE("schema registry rejects non-finite parameter defaults") {
    SECTION("NaN default") {
        SchemaRegistry registry;
        REQUIRE_FALSE(registry.registerSchema(schemaWithParameter(
            0.0, 1.0, std::numeric_limits<double>::quiet_NaN())));
    }

    SECTION("positive infinite default") {
        SchemaRegistry registry;
        REQUIRE_FALSE(registry.registerSchema(schemaWithParameter(
            0.0, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity())));
    }

    SECTION("negative infinite default") {
        SchemaRegistry registry;
        REQUIRE_FALSE(registry.registerSchema(schemaWithParameter(
            -std::numeric_limits<double>::infinity(), 0.0,
            -std::numeric_limits<double>::infinity())));
    }
}

TEST_CASE("schema registry accepts infinite bounds around a finite default") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(schemaWithParameter(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(), 0.0)));
}
