#include "nodsynth/persist/Json.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::string readAll(const char* path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

bool writeAll(const char* path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    return static_cast<bool>(output);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 1;
    const std::string instruction = readAll(argv[2]);
    if (blank(instruction)) return 2;
    std::string error;
    const auto query = nodsynth::persist::Json::parse(readAll(argv[1]), error);
    if (!query) return 1;

    auto batch = nodsynth::persist::Json::object();
    batch.set("schemaVersion", nodsynth::persist::Json::number(1));
    auto commands = nodsynth::persist::Json::array();
    const auto* notes = query->find("notes");
    static const std::vector<nodsynth::persist::Json> noNotes;
    const auto& list = notes != nullptr ? notes->asArray() : noNotes;

    if (instruction.find("harmony") != std::string::npos) {
        const auto* ppqValue = query->find("ppq");
        const int ppq = ppqValue != nullptr && ppqValue->asNumber(0) > 0 ? static_cast<int>(ppqValue->asNumber()) : 480;
        const int bar = 4 * ppq;
        const int from = 4 * bar;
        for (const auto& note : list) {
            const auto* track = note.find("track");
            if (track == nullptr || track->asString() != "bass") continue;
            const int tick = static_cast<int>(note.find("tick") != nullptr ? note.find("tick")->asNumber() : 0);
            if (tick < from) continue;
            const int index = tick / bar;
            const auto* id = note.find("id");
            auto removed = nodsynth::persist::Json::object();
            removed.set("op", nodsynth::persist::Json::string("delete-note"));
            removed.set("track", nodsynth::persist::Json::string("bass"));
            removed.set("note", nodsynth::persist::Json::string(id != nullptr ? id->asString() : ""));
            commands.push(std::move(removed));

            auto bass = nodsynth::persist::Json::object();
            bass.set("op", nodsynth::persist::Json::string("add-note"));
            bass.set("track", nodsynth::persist::Json::string("bass"));
            bass.set("clip", nodsynth::persist::Json::string("bass-clip"));
            bass.set("id", nodsynth::persist::Json::string("bass-new-" + std::to_string(index)));
            bass.set("tick", nodsynth::persist::Json::number(tick));
            bass.set("duration", nodsynth::persist::Json::number(ppq));
            bass.set("pitch", nodsynth::persist::Json::number(43));
            bass.set("velocity", nodsynth::persist::Json::number(90));
            bass.set("channel", nodsynth::persist::Json::number(0));
            commands.push(std::move(bass));

            auto harmony = nodsynth::persist::Json::object();
            harmony.set("op", nodsynth::persist::Json::string("add-note"));
            harmony.set("track", nodsynth::persist::Json::string("harmony"));
            harmony.set("clip", nodsynth::persist::Json::string("harmony-clip"));
            harmony.set("id", nodsynth::persist::Json::string("harmony-" + std::to_string(index)));
            harmony.set("tick", nodsynth::persist::Json::number(tick));
            harmony.set("duration", nodsynth::persist::Json::number(ppq * 2));
            harmony.set("pitch", nodsynth::persist::Json::number(67));
            harmony.set("velocity", nodsynth::persist::Json::number(80));
            harmony.set("channel", nodsynth::persist::Json::number(0));
            commands.push(std::move(harmony));
        }
    } else if (!list.empty()) {
        const auto& note = list.front();
        const int pitch = std::min(127, static_cast<int>(note.find("pitch") != nullptr ? note.find("pitch")->asNumber() : 0) + 2);
        auto move = nodsynth::persist::Json::object();
        move.set("op", nodsynth::persist::Json::string("move-note"));
        move.set("track", nodsynth::persist::Json::string(note.find("track") != nullptr ? note.find("track")->asString() : ""));
        move.set("note", nodsynth::persist::Json::string(note.find("id") != nullptr ? note.find("id")->asString() : ""));
        move.set("tick", nodsynth::persist::Json::number(note.find("tick") != nullptr ? note.find("tick")->asNumber() : 0));
        move.set("pitch", nodsynth::persist::Json::number(pitch));
        commands.push(std::move(move));
    }

    batch.set("commands", std::move(commands));
    return writeAll(argv[3], batch.dump()) ? 0 : 1;
}
