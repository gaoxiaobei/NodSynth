#include "AudioHost.h"

#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/GraphSession.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace model = nodsynth::model;
namespace runtime = nodsynth::runtime;
namespace persist = nodsynth::persist;

constexpr int kAudioCombo = 1001;
constexpr int kMidiCombo = 1002;
constexpr int kTimer = 1;
constexpr int kPaletteWidth = 168;
constexpr int kInspectorWidth = 248;
constexpr int kKeyboardHeight = 108;
constexpr int kStatusHeight = 26;
constexpr int kTopHeight = 40;
constexpr UINT kOpen = 11;
constexpr UINT kSave = 12;
constexpr UINT kSaveAs = 13;
constexpr UINT kQuit = 14;
constexpr UINT kSine = 21;
constexpr UINT kFilter = 22;
constexpr UINT kDelay = 23;
constexpr UINT kUndo = 31;
constexpr UINT kRedo = 32;
constexpr int kSearch = 1003;

const char* kTypes[] = {
    "nod.midi-input", "nod.note-to-frequency", "nod.oscillator", "nod.adsr", "nod.lowpass", "nod.gain",
    "nod.add", "nod.multiply", "nod.scale-bias", "nod.mix", "nod.feedback-delay", "nod.voice-mix", "nod.audio-output",
    "nod.pan-v2", "nod.lowpass-v2", "nod.highpass-v2", "nod.gain-v2", "nod.mix-v2", "nod.voice-mix-v2",
    "nod.unison-v2",
    "nod.music-delay", "nod.reverb",
};

struct KeyBinding {
    int virtualKey;
    int note;
};

constexpr KeyBinding kComputerKeys[] = {
    {'Z', 48}, {'S', 49}, {'X', 50}, {'D', 51}, {'C', 52}, {'V', 53}, {'G', 54}, {'B', 55},
    {'H', 56}, {'N', 57}, {'J', 58}, {'M', 59}, {'Q', 60}, {'2', 61}, {'W', 62}, {'3', 63},
    {'E', 64}, {'R', 65}, {'5', 66}, {'T', 67}, {'6', 68}, {'Y', 69}, {'7', 70}, {'U', 71},
};

struct PortWidget {
    std::string node;
    std::string port;
    bool output{false};
    int x{0};
    int y{0};
};

struct NodeWidget {
    std::string id;
    RECT bounds{};
};

struct SliderWidget {
    std::string node;
    std::string parameter;
    RECT bounds{};
    double minimum{0};
    double maximum{1};
    bool logarithmic{false};
};

enum class Drag { none, node, wire, pan, slider, key };

class Studio {
public:
    Studio()
        : registry_(nodsynth::nodes::builtinRegistry()),
          session_(runtime::EngineConfig{}, nodsynth::nodes::builtinRegistry(), nodsynth::nodes::builtinImplementations()) {}

    int run(HINSTANCE instance, int show);
    runtime::Engine& engine() noexcept { return session_.engine(); }

private:
    void loadPatch(model::GraphSnapshot snapshot);
    void openProject(const std::filesystem::path& path);
    void saveProject(const std::filesystem::path& path);
    void chooseAndOpen();
    void chooseAndSave(bool forceDialog);
    void restartAudio();
    void restartMidi();
    void frameGraph();
    void noteOn(int note, int velocity);
    void noteOff(int note);
    void poll();
    void paint(HDC destination, RECT client);
    void rebuildLayout(RECT client);
    void onCommand(int id);
    void onMouse(UINT message, int x, int y, WPARAM flags);
    void onKey(bool down, WPARAM key, LPARAM flags);
    std::optional<PortWidget> portAt(int x, int y) const;
    std::optional<std::string> nodeAt(int x, int y) const;
    std::optional<model::Connection> wireAt(int x, int y) const;
    model::Point toModel(int x, int y) const;
    POINT toScreen(model::Point point) const;
    RECT canvas() const;
    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    model::SchemaRegistry registry_;
    runtime::GraphSession session_;
    persist::ProjectDocument project_{persist::projectFromGraph({})};
    std::filesystem::path projectPath_;
    nodsynth::app::WasapiOutput audio_;
    nodsynth::app::MidiIn midi_;
    std::vector<nodsynth::app::AudioDevice> audioDevices_;
    std::vector<nodsynth::app::MidiDevice> midiInputs_;
    std::wstring preferredAudio_;
    int preferredMidi_{-1};
    HWND window_{nullptr};
    HWND audioCombo_{nullptr};
    HWND midiCombo_{nullptr};
    HWND searchBox_{nullptr};
    std::wstring filter_;
    std::vector<int> visibleTypes_;
    std::string status_;
    std::string telemetry_;
    std::string selected_;
    std::optional<model::Connection> selectedWire_;
    Drag drag_{Drag::none};
    std::string dragNode_;
    model::Point dragOrigin_{};
    model::Point dragGrab_{};
    PortWidget dragPort_{};
    POINT dragMouse_{};
    SliderWidget dragSlider_{};
    int dragKey_{-1};
    bool keyDown_[128]{};
    std::vector<NodeWidget> nodes_;
    std::vector<PortWidget> ports_;
    std::vector<SliderWidget> sliders_;
    std::vector<RECT> paletteRows_;
    RECT keyboard_{};
    bool fillingCombos_{false};
    std::string previewMessage_;
};

void renderAudio(void* context, float* const* channels, std::uint32_t, std::uint32_t frames) {
    static_cast<Studio*>(context)->engine().process(channels, 2, frames, {});
}

void renderMidi(void* context, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    const auto command = status & 0xf0;
    runtime::MidiEvent event;
    event.channel = status & 0x0f;
    event.data1 = data1;
    event.data2 = data2;
    if (command == 0x90 && data2 > 0) event.type = runtime::MidiType::noteOn;
    else if (command == 0x80 || (command == 0x90 && data2 == 0)) event.type = runtime::MidiType::noteOff;
    else if (command == 0xb0 && data1 == 64) event.type = runtime::MidiType::sustain;
    else if (command == 0xb0 && data1 == 123) event.type = runtime::MidiType::allNotesOff;
    else if (command == 0xb0 && data1 == 120) event.type = runtime::MidiType::allSoundOff;
    else return;
    static_cast<Studio*>(context)->engine().postDeviceMidi(event);
}

std::filesystem::path preferencesPath() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) || roaming == nullptr) {
        return {};
    }
    std::filesystem::path path = roaming;
    CoTaskMemFree(roaming);
    path /= "NodSynth";
    std::filesystem::create_directories(path);
    path /= "preferences.json";
    return path;
}

void Studio::loadPatch(model::GraphSnapshot snapshot) {
    project_ = persist::projectFromGraph(std::move(snapshot));
    session_.editor().load(project_.graph);
    frameGraph();
    projectPath_.clear();
    session_.requestCompile();
    status_ = "example loaded";
}

void Studio::frameGraph() {
    float maxX = 320.f;
    float maxY = 220.f;
    for (const auto& node : session_.editor().document().nodes()) {
        maxX = std::max(maxX, node.position.x + 220.f);
        maxY = std::max(maxY, node.position.y + 180.f);
    }
    RECT area = canvas();
    const float width = std::max(1.f, static_cast<float>(area.right - area.left - 32));
    const float height = std::max(1.f, static_cast<float>(area.bottom - area.top - 32));
    model::Viewport viewport;
    viewport.zoom = std::clamp(std::min(width / maxX, height / maxY), 0.2f, 1.f);
    viewport.originX = 24.f;
    viewport.originY = 24.f;
    session_.editor().setViewport(viewport);
}

void Studio::openProject(const std::filesystem::path& path) {
    std::string error;
    auto loaded = persist::loadProject(path, error);
    if (!loaded) {
        status_ = error.empty() ? "the project could not be opened" : error;
        return;
    }
    project_ = std::move(*loaded);
    session_.editor().load(project_.graph);
    session_.editor().setViewport(project_.viewport);
    projectPath_ = path;
    session_.requestCompile();
    status_ = "project opened";
}

void Studio::saveProject(const std::filesystem::path& path) {
    project_.graph = session_.editor().document().snapshot();
    project_.viewport = session_.editor().document().viewport();
    std::string error;
    if (!persist::saveProject(path, project_, error)) {
        status_ = error.empty() ? "the project could not be saved" : error;
        return;
    }
    projectPath_ = path;
    status_ = "project saved";
}

void Studio::chooseAndOpen() {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"NodSynth project\0*.nodsynth.json;*.json\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) openProject(buffer);
}

void Studio::chooseAndSave(bool forceDialog) {
    if (!forceDialog && !projectPath_.empty()) {
        saveProject(projectPath_);
        return;
    }
    wchar_t buffer[MAX_PATH] = L"patch.nodsynth.json";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"NodSynth project\0*.nodsynth.json\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"nodsynth.json";
    if (GetSaveFileNameW(&dialog)) saveProject(buffer);
}

void Studio::restartAudio() {
    session_.waitIdle();
    session_.poll();
    audio_.stop();
    const int index = static_cast<int>(SendMessageW(audioCombo_, CB_GETCURSEL, 0, 0));
    std::wstring deviceId;
    if (index >= 0 && index < static_cast<int>(audioDevices_.size())) deviceId = audioDevices_[static_cast<std::size_t>(index)].id;
    std::string error;
    if (!audio_.start(deviceId, &renderAudio, this, error)) {
        status_ = error.empty() ? "audio output is unavailable" : error;
        return;
    }
    runtime::PrepareConfig config = session_.engine().config();
    config.sampleRate = audio_.sampleRate();
    config.maxFrames = audio_.bufferFrames();
    config.voiceCount = 16;
    session_.engine().configure(config);
    session_.compileSync();
    audio_.begin();
    preferredAudio_ = deviceId;
    status_ = session_.sounding() ? "audio running" : session_.prepareMessage();
}

void Studio::restartMidi() {
    midi_.close();
    const int index = static_cast<int>(SendMessageW(midiCombo_, CB_GETCURSEL, 0, 0));
    preferredMidi_ = -1;
    if (index <= 0 || index - 1 >= static_cast<int>(midiInputs_.size())) {
        status_ = "MIDI input closed";
        return;
    }
    const auto& device = midiInputs_[static_cast<std::size_t>(index - 1)];
    std::string error;
    if (!midi_.open(device.index, &renderMidi, this, error)) {
        status_ = error.empty() ? "MIDI input is unavailable" : error;
        return;
    }
    preferredMidi_ = static_cast<int>(device.index);
    status_ = "MIDI input open";
}

void Studio::noteOn(int note, int velocity) {
    if (note < 0 || note > 127 || keyDown_[note]) return;
    keyDown_[note] = true;
    session_.engine().postMidi(runtime::MidiEvent{0, runtime::MidiType::noteOn, 0, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)});
}

void Studio::noteOff(int note) {
    if (note < 0 || note > 127 || !keyDown_[note]) return;
    keyDown_[note] = false;
    session_.engine().postMidi(runtime::MidiEvent{0, runtime::MidiType::noteOff, 0, static_cast<std::uint8_t>(note), 0});
}

int viewportHeight(int rows) { return 36 + rows * 22; }

RECT Studio::canvas() const {
    RECT client{};
    if (window_ == nullptr) return client;
    GetClientRect(window_, &client);
    client.left += kPaletteWidth;
    client.top += kTopHeight;
    client.right -= kInspectorWidth;
    client.bottom -= kKeyboardHeight + kStatusHeight;
    if (client.right < client.left) client.right = client.left;
    if (client.bottom < client.top) client.bottom = client.top;
    return client;
}

model::Point Studio::toModel(int x, int y) const {
    const auto viewport = session_.editor().document().viewport();
    const RECT area = canvas();
    return {(static_cast<float>(x - area.left) - viewport.originX) / viewport.zoom,
            (static_cast<float>(y - area.top) - viewport.originY) / viewport.zoom};
}

POINT Studio::toScreen(model::Point point) const {
    const auto viewport = session_.editor().document().viewport();
    const RECT area = canvas();
    return {area.left + static_cast<LONG>(point.x * viewport.zoom + viewport.originX),
            area.top + static_cast<LONG>(point.y * viewport.zoom + viewport.originY)};
}

void Studio::rebuildLayout(RECT client) {
    nodes_.clear();
    ports_.clear();
    sliders_.clear();
    paletteRows_.clear();
    visibleTypes_.clear();
    const RECT area = canvas();
    const auto& document = session_.editor().document();
    for (const auto& node : document.nodes()) {
        const auto* schema = registry_.find(node.typeId);
        model::Point position = node.position;
        if (drag_ == Drag::node && node.id.value == dragNode_) position = dragOrigin_;
        const POINT origin = toScreen(position);
        int inputs = 0;
        int outputs = 0;
        if (schema != nullptr) {
            for (const auto& port : schema->ports) {
                if (port.direction == model::PortDirection::input) ++inputs;
                else ++outputs;
            }
        }
        const int rows = std::max(1, std::max(inputs, outputs));
        const float zoom = std::max(0.2f, document.viewport().zoom);
        const int height = std::max(28, static_cast<int>(viewportHeight(rows) * zoom));
        NodeWidget widget;
        widget.id = node.id.value;
        widget.bounds = {origin.x, origin.y, origin.x + static_cast<int>(180 * document.viewport().zoom), origin.y + height};
        int inputRow = 0;
        int outputRow = 0;
        if (schema != nullptr) {
            for (const auto& port : schema->ports) {
                const bool output = port.direction == model::PortDirection::output;
                const int row = output ? outputRow++ : inputRow++;
                PortWidget hit;
                hit.node = node.id.value;
                hit.port = port.id.value;
                hit.output = output;
                hit.x = output ? widget.bounds.right : widget.bounds.left;
                hit.y = widget.bounds.top + static_cast<int>(28 * document.viewport().zoom) + row * static_cast<int>(22 * document.viewport().zoom) + 8;
                ports_.push_back(hit);
            }
        }
        nodes_.push_back(widget);
    }
    int row = 0;
    for (int index = 0; index < static_cast<int>(std::size(kTypes)); ++index) {
        const auto* schema = registry_.find(model::NodeTypeId{kTypes[index]});
        const char* label = schema != nullptr ? schema->displayName.c_str() : kTypes[index];
        if (!filter_.empty()) {
            std::wstring folded = filter_;
            for (auto& character : folded) character = static_cast<wchar_t>(towlower(character));
            std::wstring name;
            for (const char* cursor = label; *cursor != '\0'; ++cursor) name.push_back(static_cast<wchar_t>(towlower(static_cast<unsigned char>(*cursor))));
            if (name.find(folded) == std::wstring::npos) continue;
        }
        RECT bounds{8, kTopHeight + 8 + row * 28, kPaletteWidth - 8, kTopHeight + 32 + row * 28};
        paletteRows_.push_back(bounds);
        visibleTypes_.push_back(index);
        ++row;
    }
    const auto* selected = document.findNode(model::NodeId{selected_});
    if (selected != nullptr) {
        const auto* schema = registry_.find(selected->typeId);
        if (schema != nullptr) {
            int index = 0;
            for (const auto& parameter : schema->parameters) {
                SliderWidget slider;
                slider.node = selected->id.value;
                slider.parameter = parameter.id.value;
                slider.minimum = parameter.minimum;
                slider.maximum = parameter.maximum;
                slider.logarithmic = parameter.scale == model::ParameterScale::logarithmic;
                const int top = kTopHeight + 48 + index * 48;
                slider.bounds = {client.right - kInspectorWidth + 16, top, client.right - 16, top + 18};
                sliders_.push_back(slider);
                ++index;
            }
        }
    }
    keyboard_ = {kPaletteWidth, client.bottom - kKeyboardHeight, client.right - kInspectorWidth, client.bottom};
    (void)area;
}

std::optional<PortWidget> Studio::portAt(int x, int y) const {
    for (auto it = ports_.rbegin(); it != ports_.rend(); ++it) {
        const int dx = x - it->x;
        const int dy = y - it->y;
        if (dx * dx + dy * dy <= 12 * 12) return *it;
    }
    return std::nullopt;
}

std::optional<std::string> Studio::nodeAt(int x, int y) const {
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (PtInRect(&it->bounds, POINT{x, y})) return it->id;
    }
    return std::nullopt;
}

std::optional<model::Connection> Studio::wireAt(int x, int y) const {
    for (const auto& connection : session_.editor().document().connections()) {
        const PortWidget* from = nullptr;
        const PortWidget* to = nullptr;
        for (const auto& port : ports_) {
            if (port.node == connection.from.nodeId.value && port.port == connection.from.portId.value && port.output) from = &port;
            if (port.node == connection.to.nodeId.value && port.port == connection.to.portId.value && !port.output) to = &port;
        }
        if (from == nullptr || to == nullptr) continue;
        const float length = std::hypot(static_cast<float>(to->x - from->x), static_cast<float>(to->y - from->y));
        if (length < 1.f) continue;
        const float t = std::clamp(((x - from->x) * (to->x - from->x) + (y - from->y) * (to->y - from->y)) / (length * length), 0.f, 1.f);
        const float px = from->x + (to->x - from->x) * t;
        const float py = from->y + (to->y - from->y) * t;
        if (std::hypot(px - x, py - y) <= 6.f) return connection;
    }
    return std::nullopt;
}

void Studio::poll() {
    session_.poll();
    session_.engine().reclaim();
    const auto telemetry = session_.engine().telemetry();
    char text[160];
    std::snprintf(text, sizeof(text), "voices %u   cpu p50 %.0f%%  p99 %.0f%%   xruns %u", telemetry.activeVoices,
                  telemetry.loadP50 * 100.f, telemetry.loadP99 * 100.f, telemetry.xruns);
    telemetry_ = text;
    InvalidateRect(window_, nullptr, FALSE);
}

void Studio::paint(HDC destination, RECT client) {
    rebuildLayout(client);
    HDC memory = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, client.right, std::max(1L, client.bottom));
    HGDIOBJ previous = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(22, 24, 29));
    FillRect(memory, &client, background);
    DeleteObject(background);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(220, 224, 230));
    int row = 0;
    for (const int typeIndex : visibleTypes_) {
        const RECT bounds = paletteRows_[static_cast<std::size_t>(row++)];
        HBRUSH brush = CreateSolidBrush(RGB(42, 47, 56));
        FillRect(memory, &bounds, brush);
        DeleteObject(brush);
        const auto* schema = registry_.find(model::NodeTypeId{kTypes[typeIndex]});
        const char* label = schema != nullptr ? schema->displayName.c_str() : kTypes[typeIndex];
        TextOutA(memory, bounds.left + 8, bounds.top + 6, label, static_cast<int>(std::strlen(label)));
    }
    const RECT area = canvas();
    HBRUSH canvasBrush = CreateSolidBrush(RGB(16, 17, 21));
    FillRect(memory, &area, canvasBrush);
    DeleteObject(canvasBrush);
    HPEN wirePen = CreatePen(PS_SOLID, 2, RGB(130, 156, 178));
    HGDIOBJ oldPen = SelectObject(memory, wirePen);
    for (const auto& connection : session_.editor().document().connections()) {
        const PortWidget* from = nullptr;
        const PortWidget* to = nullptr;
        for (const auto& port : ports_) {
            if (port.output && port.node == connection.from.nodeId.value && port.port == connection.from.portId.value) from = &port;
            if (!port.output && port.node == connection.to.nodeId.value && port.port == connection.to.portId.value) to = &port;
        }
        if (from == nullptr || to == nullptr) continue;
        MoveToEx(memory, from->x, from->y, nullptr);
        LineTo(memory, to->x, to->y);
    }
    if (drag_ == Drag::wire) {
        MoveToEx(memory, dragPort_.x, dragPort_.y, nullptr);
        LineTo(memory, dragMouse_.x, dragMouse_.y);
    }
    SelectObject(memory, oldPen);
    DeleteObject(wirePen);
    for (const auto& widget : nodes_) {
        const auto* node = session_.editor().document().findNode(model::NodeId{widget.id});
        const bool selected = widget.id == selected_;
        HBRUSH brush = CreateSolidBrush(selected ? RGB(58, 68, 84) : RGB(40, 44, 52));
        HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? RGB(232, 196, 104) : RGB(78, 86, 98));
        HGDIOBJ oldBrush = SelectObject(memory, brush);
        HGDIOBJ oldNodePen = SelectObject(memory, pen);
        RoundRect(memory, widget.bounds.left, widget.bounds.top, widget.bounds.right, widget.bounds.bottom, 8, 8);
        SelectObject(memory, oldBrush);
        SelectObject(memory, oldNodePen);
        DeleteObject(brush);
        DeleteObject(pen);
        if (node != nullptr) {
            const auto* schema = registry_.find(node->typeId);
            const std::string title = schema != nullptr ? schema->displayName : node->id.value;
            TextOutA(memory, widget.bounds.left + 10, widget.bounds.top + 6, title.c_str(), static_cast<int>(title.size()));
        }
    }
    for (const auto& port : ports_) {
        HBRUSH brush = CreateSolidBrush(port.output ? RGB(116, 186, 148) : RGB(214, 164, 104));
        HGDIOBJ oldBrush = SelectObject(memory, brush);
        Ellipse(memory, port.x - 5, port.y - 5, port.x + 5, port.y + 5);
        SelectObject(memory, oldBrush);
        DeleteObject(brush);
    }
    TextOutA(memory, client.right - kInspectorWidth + 16, kTopHeight + 12, "Inspector", 9);
    const auto* selected = session_.editor().document().findNode(model::NodeId{selected_});
    if (selected != nullptr) {
        TextOutA(memory, client.right - kInspectorWidth + 16, kTopHeight + 30, selected->id.value.c_str(), static_cast<int>(selected->id.value.size()));
    }
    for (const auto& slider : sliders_) {
        const auto* node = session_.editor().document().findNode(model::NodeId{slider.node});
        double value = slider.minimum;
        if (node != nullptr) {
            const auto found = node->parameters.find(model::ParameterId{slider.parameter});
            if (found != node->parameters.end()) value = found->second;
        }
        double fraction = 0;
        if (slider.logarithmic && slider.minimum > 0 && value > 0 && slider.maximum > slider.minimum) {
            fraction = std::log(value / slider.minimum) / std::log(slider.maximum / slider.minimum);
        } else if (slider.maximum > slider.minimum) {
            fraction = (value - slider.minimum) / (slider.maximum - slider.minimum);
        }
        fraction = std::clamp(fraction, 0.0, 1.0);
        HBRUSH track = CreateSolidBrush(RGB(32, 36, 42));
        FillRect(memory, &slider.bounds, track);
        DeleteObject(track);
        RECT filled = slider.bounds;
        filled.right = filled.left + static_cast<int>((filled.right - filled.left) * fraction);
        HBRUSH fill = CreateSolidBrush(RGB(214, 164, 104));
        FillRect(memory, &filled, fill);
        DeleteObject(fill);
        const std::string label = slider.parameter;
        TextOutA(memory, slider.bounds.left, slider.bounds.top - 16, label.c_str(), static_cast<int>(label.size()));
    }
    HBRUSH keyBrush = CreateSolidBrush(RGB(236, 236, 232));
    HBRUSH blackBrush = CreateSolidBrush(RGB(28, 30, 34));
    HBRUSH heldBrush = CreateSolidBrush(RGB(232, 196, 104));
    const int whiteCount = 15;
    const int keyWidth = std::max(8, static_cast<int>(keyboard_.right - keyboard_.left) / whiteCount);
    int whiteIndex = 0;
    for (int note = 48; note <= 72; ++note) {
        const int pitch = note % 12;
        const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
        if (black) continue;
        RECT key{keyboard_.left + whiteIndex * keyWidth, keyboard_.top + 8, keyboard_.left + (whiteIndex + 1) * keyWidth - 2, keyboard_.bottom - 8};
        FillRect(memory, &key, keyDown_[note] ? heldBrush : keyBrush);
        ++whiteIndex;
    }
    whiteIndex = 0;
    for (int note = 48; note <= 72; ++note) {
        const int pitch = note % 12;
        const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
        if (!black) {
            ++whiteIndex;
            continue;
        }
        RECT key{keyboard_.left + whiteIndex * keyWidth - keyWidth / 3, keyboard_.top + 8,
                 keyboard_.left + whiteIndex * keyWidth + keyWidth / 3, keyboard_.top + (keyboard_.bottom - keyboard_.top) / 2};
        FillRect(memory, &key, keyDown_[note] ? heldBrush : blackBrush);
    }
    DeleteObject(keyBrush);
    DeleteObject(blackBrush);
    DeleteObject(heldBrush);
    RECT status{0, client.bottom - kKeyboardHeight - kStatusHeight, client.right, client.bottom - kKeyboardHeight};
    std::string line = telemetry_;
    if (!status_.empty()) line = status_ + "   " + telemetry_;
    if (!session_.diagnostics().empty()) line = session_.diagnostics().front().message + "   " + telemetry_;
    if (!session_.prepareMessage().empty()) line = session_.prepareMessage() + "   " + telemetry_;
    if (!previewMessage_.empty()) line = previewMessage_;
    TextOutA(memory, 12, status.top + 4, line.c_str(), static_cast<int>(line.size()));
    BitBlt(destination, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

int noteAt(RECT keyboard, int x, int y) {
    const int whiteCount = 15;
    const int keyWidth = std::max(8, static_cast<int>(keyboard.right - keyboard.left) / whiteCount);
    int whiteIndex = 0;
    for (int note = 48; note <= 72; ++note) {
        const int pitch = note % 12;
        const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
        if (!black) {
            ++whiteIndex;
            continue;
        }
        RECT key{keyboard.left + whiteIndex * keyWidth - keyWidth / 3, keyboard.top + 8,
                 keyboard.left + whiteIndex * keyWidth + keyWidth / 3, keyboard.top + (keyboard.bottom - keyboard.top) / 2};
        if (PtInRect(&key, POINT{x, y})) return note;
    }
    if (y < keyboard.top) return -1;
    const int index = (x - keyboard.left) / keyWidth;
    if (index < 0 || index >= whiteCount) return -1;
    int seen = 0;
    for (int note = 48; note <= 72; ++note) {
        const int pitch = note % 12;
        if (pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10) continue;
        if (seen == index) return note;
        ++seen;
    }
    return -1;
}

double sliderValue(const SliderWidget& slider, int x) {
    const double fraction = std::clamp(static_cast<double>(x - slider.bounds.left) / std::max(1, static_cast<int>(slider.bounds.right - slider.bounds.left)), 0.0, 1.0);
    if (slider.logarithmic && slider.minimum > 0 && slider.maximum > slider.minimum) {
        return slider.minimum * std::pow(slider.maximum / slider.minimum, fraction);
    }
    return slider.minimum + (slider.maximum - slider.minimum) * fraction;
}

void Studio::onMouse(UINT message, int x, int y, WPARAM) {
    if (message == WM_LBUTTONDOWN) {
        SetFocus(window_);
        SetCapture(window_);
        if (const int note = noteAt(keyboard_, x, y); note >= 0 && y >= keyboard_.top) {
            drag_ = Drag::key;
            dragKey_ = note;
            noteOn(note, 100);
            return;
        }
        for (std::size_t index = 0; index < paletteRows_.size(); ++index) {
            if (!PtInRect(&paletteRows_[index], POINT{x, y})) continue;
            if (index >= visibleTypes_.size()) continue;
            const auto* schema = registry_.find(model::NodeTypeId{kTypes[visibleTypes_[index]]});
            if (schema == nullptr) return;
            model::NodeRecord record;
            record.typeId = schema->typeId;
            record.schemaVersion = schema->schemaVersion;
            for (int suffix = 1;; ++suffix) {
                record.id = model::NodeId{schema->displayName + "-" + std::to_string(suffix)};
                if (session_.editor().document().findNode(record.id) == nullptr) break;
            }
            for (const auto& parameter : schema->parameters) record.parameters[parameter.id] = parameter.defaultValue;
            const RECT area = canvas();
            record.position = toModel(area.left + 80 + static_cast<int>(index % 4) * 24, area.top + 80 + static_cast<int>(index % 4) * 24);
            if (session_.editor().addNode(record)) {
                selected_ = record.id.value;
                session_.requestCompile();
            }
            drag_ = Drag::none;
            ReleaseCapture();
            return;
        }
        for (const auto& slider : sliders_) {
            RECT hit = slider.bounds;
            hit.top -= 18;
            if (!PtInRect(&hit, POINT{x, y})) continue;
            drag_ = Drag::slider;
            dragSlider_ = slider;
            session_.setParameter(model::NodeId{slider.node}, model::ParameterId{slider.parameter}, sliderValue(slider, x));
            return;
        }
        if (const auto port = portAt(x, y); port && port->output) {
            drag_ = Drag::wire;
            dragPort_ = *port;
            dragMouse_ = {x, y};
            previewMessage_.clear();
            return;
        }
        if (const auto wire = wireAt(x, y)) {
            selectedWire_ = wire;
            selected_.clear();
            drag_ = Drag::none;
            ReleaseCapture();
            return;
        }
        if (const auto node = nodeAt(x, y)) {
            selected_ = *node;
            selectedWire_.reset();
            const auto* record = session_.editor().document().findNode(model::NodeId{*node});
            drag_ = Drag::node;
            dragNode_ = *node;
            dragGrab_ = toModel(x, y);
            dragOrigin_ = record != nullptr ? record->position : model::Point{};
            return;
        }
        const RECT area = canvas();
        if (PtInRect(&area, POINT{x, y})) {
            drag_ = Drag::pan;
            dragMouse_ = {x, y};
            selected_.clear();
        }
        return;
    }
    if (message == WM_MOUSEMOVE) {
        if (drag_ == Drag::node) {
            const model::Point current = toModel(x, y);
            dragOrigin_.x += current.x - dragGrab_.x;
            dragOrigin_.y += current.y - dragGrab_.y;
            dragGrab_ = current;
        } else if (drag_ == Drag::wire) {
            dragMouse_ = {x, y};
            previewMessage_.clear();
            if (const auto port = portAt(x, y); port && !port->output) {
                const auto preview = session_.preview(model::Connection{
                    {model::NodeId{dragPort_.node}, model::PortId{dragPort_.port}},
                    {model::NodeId{port->node}, model::PortId{port->port}}});
                if (!preview.diagnostics.empty()) previewMessage_ = preview.diagnostics.front().message;
            }
        } else if (drag_ == Drag::pan) {
            auto viewport = session_.editor().document().viewport();
            viewport.originX += static_cast<float>(x - dragMouse_.x);
            viewport.originY += static_cast<float>(y - dragMouse_.y);
            session_.editor().setViewport(viewport);
            dragMouse_ = {x, y};
        } else if (drag_ == Drag::slider) {
            session_.setParameter(model::NodeId{dragSlider_.node}, model::ParameterId{dragSlider_.parameter}, sliderValue(dragSlider_, x));
        } else if (drag_ == Drag::key) {
            const int note = noteAt(keyboard_, x, y);
            if (note != dragKey_) {
                noteOff(dragKey_);
                dragKey_ = note;
                if (note >= 0) noteOn(note, 100);
            }
        }
        return;
    }
    if (message == WM_LBUTTONUP) {
        if (drag_ == Drag::node) session_.editor().moveNode(model::NodeId{dragNode_}, dragOrigin_);
        if (drag_ == Drag::wire) {
            if (const auto port = portAt(x, y); port && !port->output) {
                model::Connection connection{{model::NodeId{dragPort_.node}, model::PortId{dragPort_.port}},
                                              {model::NodeId{port->node}, model::PortId{port->port}}};
                if (session_.editor().connect(connection)) session_.requestCompile();
                else status_ = "that input is already connected";
            }
        }
        if (drag_ == Drag::key) noteOff(dragKey_);
        previewMessage_.clear();
        drag_ = Drag::none;
        ReleaseCapture();
        return;
    }
    if (message == WM_RBUTTONUP) {
        if (const auto wire = wireAt(x, y)) {
            if (session_.editor().disconnect(*wire)) session_.requestCompile();
            if (selectedWire_ && *selectedWire_ == *wire) selectedWire_.reset();
        }
    }
}

void Studio::onKey(bool down, WPARAM key, LPARAM flags) {
    if (down && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (key == 'Z') session_.undo();
        if (key == 'Y') session_.redo();
        if (key == 'S') chooseAndSave(false);
        return;
    }
    if (down && key == VK_DELETE) {
        if (!selected_.empty()) {
            session_.editor().removeNode(model::NodeId{selected_});
            selected_.clear();
            session_.requestCompile();
        } else if (selectedWire_) {
            session_.editor().disconnect(*selectedWire_);
            selectedWire_.reset();
            session_.requestCompile();
        }
        return;
    }
    if (down && (flags & (1 << 30))) return;
    for (const auto& binding : kComputerKeys) {
        if (binding.virtualKey != static_cast<int>(key)) continue;
        if (down) noteOn(binding.note, 100);
        else noteOff(binding.note);
    }
}

void Studio::onCommand(int id) {
    if (id == kAudioCombo + (CBN_SELCHANGE << 16)) {
        if (!fillingCombos_) restartAudio();
        return;
    }
    if (id == kMidiCombo + (CBN_SELCHANGE << 16)) {
        if (!fillingCombos_) restartMidi();
        return;
    }
    switch (static_cast<UINT>(id)) {
    case kOpen: chooseAndOpen(); break;
    case kSave: chooseAndSave(false); break;
    case kSaveAs: chooseAndSave(true); break;
    case kQuit: DestroyWindow(window_); break;
    case kSine: loadPatch(nodsynth::nodes::sinePatch()); break;
    case kFilter: loadPatch(nodsynth::nodes::filterPatch()); break;
    case kDelay: loadPatch(nodsynth::nodes::delayPatch()); break;
    case kUndo: session_.undo(); break;
    case kRedo: session_.redo(); break;
    default: break;
    }
}

LRESULT Studio::handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == kSearch && HIWORD(wparam) == EN_CHANGE) {
            wchar_t buffer[128]{};
            GetWindowTextW(searchBox_, buffer, 128);
            filter_ = buffer;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        onCommand(LOWORD(wparam) == kAudioCombo || LOWORD(wparam) == kMidiCombo ? static_cast<int>(LOWORD(wparam) + (HIWORD(wparam) << 16)) : LOWORD(wparam));
        return 0;
    case WM_TIMER: poll(); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: onMouse(message, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), wparam); return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        auto viewport = session_.editor().document().viewport();
        const float next = std::clamp(viewport.zoom * (delta > 0 ? 1.1f : 0.9f), 0.2f, 4.f);
        viewport.zoom = next;
        session_.editor().setViewport(viewport);
        return 0;
    }
    case WM_KEYDOWN: onKey(true, wparam, lparam); return 0;
    case WM_KEYUP: onKey(false, wparam, lparam); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paintStruct;
        HDC dc = BeginPaint(window, &paintStruct);
        RECT client{};
        GetClientRect(window, &client);
        paint(dc, client);
        EndPaint(window, &paintStruct);
        return 0;
    }
    case WM_SIZE: {
        RECT client{};
        GetClientRect(window, &client);
        MoveWindow(searchBox_, 8, 8, kPaletteWidth - 16, 24, TRUE);
        MoveWindow(audioCombo_, kPaletteWidth, 8, 280, 240, TRUE);
        MoveWindow(midiCombo_, kPaletteWidth + 292, 8, 220, 240, TRUE);
        (void)client;
        return 0;
    }
    case WM_DESTROY:
        audio_.stop();
        midi_.close();
        KillTimer(window, kTimer);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK Studio::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* studio = reinterpret_cast<Studio*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (studio == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    return studio->handle(window, message, wparam, lparam);
}

void loadPreferences(std::wstring& audio, int& midi) {
    const auto path = preferencesPath();
    std::ifstream input(path);
    if (!input) return;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string error;
    auto json = persist::Json::parse(text, error);
    if (!json) return;
    if (const auto* device = json->find("audioDevice")) {
        const auto& value = device->asString();
        audio.assign(value.begin(), value.end());
    }
    if (const auto* device = json->find("midiDevice")) midi = static_cast<int>(device->asNumber(-1));
}

void savePreferences(const std::wstring& audio, int midi) {
    const auto path = preferencesPath();
    if (path.empty()) return;
    auto json = persist::Json::object();
    std::string device(audio.begin(), audio.end());
    json.set("audioDevice", persist::Json::string(std::move(device)));
    json.set("midiDevice", persist::Json::number(midi));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) output << json.dump();
}

int Studio::run(HINSTANCE instance, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadPreferences(preferredAudio_, preferredMidi_);
    loadPatch(nodsynth::nodes::sinePatch());
    session_.compileSync();
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &Studio::procedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"NodSynthStudio";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(RGB(22, 24, 29));
    RegisterClassW(&windowClass);
    window_ = CreateWindowExW(0, windowClass.lpszClassName, L"NodSynth", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                               nullptr, nullptr, instance, this);
    frameGraph();
    audioCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 100, 200, window_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioCombo)), instance, nullptr);
    midiCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 100, 200, window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMidiCombo)), instance, nullptr);
    searchBox_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 8, 8, kPaletteWidth - 16, 24, window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearch)), instance, nullptr);
    SendMessageW(searchBox_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search nodes"));
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kOpen, L"Open");
    AppendMenuW(file, MF_STRING, kSave, L"Save");
    AppendMenuW(file, MF_STRING, kSaveAs, L"Save As");
    AppendMenuW(file, MF_STRING, kQuit, L"Exit");
    HMENU examples = CreatePopupMenu();
    AppendMenuW(examples, MF_STRING, kSine, L"Sine");
    AppendMenuW(examples, MF_STRING, kFilter, L"Filter");
    AppendMenuW(examples, MF_STRING, kDelay, L"Delay");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, kUndo, L"Undo");
    AppendMenuW(edit, MF_STRING, kRedo, L"Redo");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(examples), L"Examples");
    SetMenu(window_, menu);
    audioDevices_ = nodsynth::app::renderDevices();
    midiInputs_ = nodsynth::app::midiDevices();
    fillingCombos_ = true;
    int audioSelection = 0;
    for (std::size_t index = 0; index < audioDevices_.size(); ++index) {
        SendMessageW(audioCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(audioDevices_[index].name.c_str()));
        if (audioDevices_[index].id == preferredAudio_) audioSelection = static_cast<int>(index);
    }
    if (audioDevices_.empty()) SendMessageW(audioCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No audio device"));
    SendMessageW(audioCombo_, CB_SETCURSEL, audioSelection, 0);
    SendMessageW(midiCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No MIDI input"));
    int midiSelection = 0;
    for (std::size_t index = 0; index < midiInputs_.size(); ++index) {
        SendMessageW(midiCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(midiInputs_[index].name.c_str()));
        if (static_cast<int>(midiInputs_[index].index) == preferredMidi_) midiSelection = static_cast<int>(index + 1);
    }
    SendMessageW(midiCombo_, CB_SETCURSEL, midiSelection, 0);
    fillingCombos_ = false;
    ShowWindow(window_, show);
    if (!audioDevices_.empty()) restartAudio();
    if (midiSelection > 0) restartMidi();
    savePreferences(preferredAudio_, preferredMidi_);
    SetTimer(window_, kTimer, 33, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    savePreferences(preferredAudio_, preferredMidi_);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

void renderSelf(void* context, float* const* channels, std::uint32_t, std::uint32_t frames) {
    static_cast<runtime::GraphSession*>(context)->engine().process(channels, 2, frames, {});
}

int selfTest() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    runtime::GraphSession session(runtime::EngineConfig{}, nodsynth::nodes::builtinRegistry(), nodsynth::nodes::builtinImplementations());
    session.editor().load(nodsynth::nodes::sinePatch());
    session.compileSync();
    if (!session.sounding()) return 1;
    std::vector<float> left(256, 0.f);
    std::vector<float> right(256, 0.f);
    float* outputs[] = {left.data(), right.data()};
    const runtime::MidiEvent note{0, runtime::MidiType::noteOn, 0, 69, 100};
    session.engine().process(outputs, 2, 128, std::span<const runtime::MidiEvent>(&note, 1));
    for (int block = 0; block < 30; ++block) session.engine().process(outputs, 2, 128, {});
    float peak = 0.f;
    for (float sample : left) peak = std::max(peak, std::fabs(sample));
    if (!std::isfinite(peak) || peak < 0.01f) return 1;
    const auto path = std::filesystem::temp_directory_path() / "nodsynth-self-test.nodsynth.json";
    auto document = persist::projectFromGraph(session.editor().document().snapshot(), session.editor().document().viewport());
    std::string error;
    if (!persist::saveProject(path, document, error)) return 1;
    auto loaded = persist::loadProject(path, error);
    if (!loaded || loaded->graph.nodes.size() != session.editor().document().nodes().size()) return 1;
    auto devices = nodsynth::app::renderDevices();
    if (!devices.empty()) {
        nodsynth::app::WasapiOutput output;
        if (!output.start({}, &renderSelf, &session, error)) return 2;
        runtime::PrepareConfig config = session.engine().config();
        config.sampleRate = output.sampleRate();
        config.maxFrames = output.bufferFrames();
        session.engine().configure(config);
        session.compileSync();
        if (!session.sounding()) return 3;
        output.begin();
        Sleep(250);
        output.stop();
    }
    CoUninitialize();
    return 0;
}

int liveSoak() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    runtime::GraphSession session(runtime::EngineConfig{}, nodsynth::nodes::builtinRegistry(), nodsynth::nodes::builtinImplementations());
    session.editor().load(nodsynth::nodes::sinePatch());
    session.compileSync();
    nodsynth::app::WasapiOutput output;
    std::string error;
    const auto report = [&](int code) {
        std::ofstream file("live-soak.txt", std::ios::trunc);
        const auto telemetry = session.engine().telemetry();
        file << "code=" << code << " rate=" << output.sampleRate() << " frames=" << output.bufferFrames()
             << " xruns=" << telemetry.xruns << " voices=" << telemetry.activeVoices << " p50=" << telemetry.loadP50
             << " p99=" << telemetry.loadP99 << " last=" << telemetry.lastLoad << " error=" << error << '\n';
        CoUninitialize();
        return code;
    };
    if (!session.sounding()) return report(1);
    if (!output.start({}, &renderSelf, &session, error)) return report(2);
    runtime::PrepareConfig config = session.engine().config();
    config.sampleRate = output.sampleRate();
    config.maxFrames = output.bufferFrames();
    session.engine().configure(config);
    session.compileSync();
    if (!session.sounding()) return report(3);
    session.setParameter(model::NodeId{"gain"}, model::ParameterId{"gain"}, 0.15);
    output.begin();
    for (std::uint8_t note = 48; note < 64; ++note) {
        session.engine().postMidi(runtime::MidiEvent{0, runtime::MidiType::noteOn, 0, note, 96});
    }
    Sleep(600000);
    for (std::uint8_t note = 48; note < 64; ++note) {
        session.engine().postMidi(runtime::MidiEvent{0, runtime::MidiType::noteOff, 0, note, 0});
    }
    Sleep(2000);
    const auto telemetry = session.engine().telemetry();
    output.stop();
    if (telemetry.xruns != 0 || telemetry.activeVoices != 0) return report(4);
    return report(0);
}

int launch(HINSTANCE instance, PWSTR commandLine, int show) {
    if (commandLine != nullptr && std::wcsstr(commandLine, L"--live-soak") != nullptr) return liveSoak();
    if (commandLine != nullptr && std::wcsstr(commandLine, L"--self-test") != nullptr) return selfTest();
    Studio studio;
    return studio.run(instance, show);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    return launch(instance, commandLine, show);
}
