#include <nodsynth/daw/Timeline.h>
#include <nodsynth/persist/Json.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/song/SongDocument.h>
#include <nodsynth/song/SongRenderer.h>

#include "../win32/AudioHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
nodsynth::song::SongDocument gSong;
std::vector<nodsynth::daw::NoteBox> gNotes;
std::wstring gPath;
bool gPlaying = false;
std::vector<float> gAudio;
std::uint32_t gSampleRate = 48000;
std::uint64_t gPlaySample = 0;
nodsynth::app::WasapiOutput gOutput;
const nodsynth::daw::NoteBox* gDrag = nullptr;
enum class DragKind { none, note, gain, pan };
DragKind gDragKind = DragKind::none;
std::size_t gDragStrip = 0;
std::size_t gSelected = 0;
std::vector<nodsynth::daw::MixerStrip> gStrips;
HWND gPanel = nullptr;
constexpr int kPanelHeight = 168;
constexpr int kMixerHeight = 176;

nodsynth::daw::ViewMetrics pianoTime()
{
    nodsynth::daw::ViewMetrics metrics;
    metrics.headerWidth = 56;
    metrics.pixelsPerQuarter = 48;
    return metrics;
}

int mixerTopFor(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);
    const int content = client.bottom > kPanelHeight ? client.bottom - kPanelHeight : 0;
    return content > kMixerHeight ? content - kMixerHeight : 0;
}

void refreshPanel()
{
    if (gPanel == nullptr) return;
    const auto text = nodsynth::song::querySong(gSong).dump();
    SetWindowTextA(gPanel, text.c_str());
}

void layoutPanel(HWND window)
{
    if (gPanel == nullptr) return;
    RECT client{};
    GetClientRect(window, &client);
    const int top = client.bottom > kPanelHeight ? client.bottom - kPanelHeight : 0;
    MoveWindow(gPanel, 0, top, client.right, kPanelHeight, TRUE);
}

void reloadNotes()
{
    if (gSong.tracks.empty()) gSelected = 0;
    else if (gSelected >= gSong.tracks.size()) gSelected = gSong.tracks.size() - 1;
    gNotes = nodsynth::daw::layoutPianoRoll(gSong, gSelected);
    gStrips = nodsynth::daw::layoutMixer(gSong);
}

bool applyBatch(const nodsynth::persist::Json& batch)
{
    const auto applied = nodsynth::song::applyCommands(gSong, batch, gSong.revision);
    if (!applied.ok) return false;
    reloadNotes();
    refreshPanel();
    return true;
}

void paint(HWND window)
{
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    const int mixerTop = mixerTopFor(window);
    nodsynth::daw::PianoRollMetrics piano;
    for (int pitch = piano.lowPitch; pitch <= piano.highPitch; ++pitch) {
        if (pitch % 12 != 0) continue;
        const int y = (static_cast<int>(piano.highPitch) - pitch) * piano.rowHeight;
        char label[8];
        std::snprintf(label, sizeof(label), "C%d", pitch / 12 - 1);
        TextOutA(dc, 4, y, label, static_cast<int>(std::strlen(label)));
    }
    HBRUSH noteBrush = CreateSolidBrush(RGB(40, 90, 180));
    for (const auto& note : gNotes) {
        if (note.y >= mixerTop) continue;
        RECT box{note.x, note.y, note.x + note.width, note.y + note.height};
        FillRect(dc, &box, noteBrush);
    }
    DeleteObject(noteBrush);
    const int playhead = nodsynth::daw::playheadPixel(gSong, static_cast<std::int64_t>(gPlaySample), gSampleRate, pianoTime());
    MoveToEx(dc, playhead, 0, nullptr);
    LineTo(dc, playhead, mixerTop);
    RECT mixer{0, mixerTop, client.right, mixerTop + kMixerHeight};
    FillRect(dc, &mixer, reinterpret_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    nodsynth::daw::MixerMetrics mixerMetrics;
    for (std::size_t index = 0; index < gStrips.size() && index < gSong.tracks.size(); ++index) {
        const auto& strip = gStrips[index];
        const auto& track = gSong.tracks[index];
        RECT body{strip.x + 4, mixerTop + strip.y + 4, strip.x + strip.width - 4, mixerTop + strip.height - 4};
        FillRect(dc, &body, reinterpret_cast<HBRUSH>(GetStockObject(index == gSelected ? WHITE_BRUSH : GRAY_BRUSH)));
        const auto& name = track.name.empty() ? track.id : track.name;
        TextOutA(dc, strip.x + 6, mixerTop + 6, name.c_str(), static_cast<int>(name.size()));
        const int faderTravel = mixerMetrics.faderHeight;
        const int thumb = mixerTop + mixerMetrics.faderTop + static_cast<int>((1.0 - std::clamp(track.gain, 0.0, 2.0) / 2.0) * faderTravel);
        RECT fader{strip.x + strip.width / 2 - 4, thumb, strip.x + strip.width / 2 + 4, thumb + 8};
        FillRect(dc, &fader, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const int pan = strip.x + static_cast<int>((std::clamp(track.pan, -1.0, 1.0) + 1.0) * 0.5 * strip.width);
        RECT panMark{pan - 2, mixerTop + strip.height - 16, pan + 2, mixerTop + strip.height - 8};
        FillRect(dc, &panMark, reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
    }
    EndPaint(window, &paint);
}

void renderCallback(void*, float* const* channels, std::uint32_t channelCount, std::uint32_t frames)
{
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto cursor = static_cast<std::size_t>(gPlaySample + frame) * 2;
        const float left = cursor + 1 < gAudio.size() ? gAudio[cursor] : 0.f;
        const float right = cursor + 1 < gAudio.size() ? gAudio[cursor + 1] : 0.f;
        if (channelCount > 0) channels[0][frame] = left;
        if (channelCount > 1) channels[1][frame] = right;
    }
    gPlaySample += frames;
    if (gPlaySample * 2 >= gAudio.size()) gPlaying = false;
}

void stopPlayback()
{
    gPlaying = false;
    gOutput.stop();
    gPlaySample = 0;
}

bool startPlayback(HWND window)
{
    if (gSong.tracks.empty()) return false;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto wavPath = std::filesystem::path(temp) / L"nodsynth-daw-play.wav";
    nodsynth::song::SongRenderOptions options;
    options.tailSeconds = 0.4;
    options.baseDirectory = std::filesystem::path(gPath).parent_path();
    const auto report = nodsynth::song::renderSong(gSong, options, wavPath);
    if (!report.ok) return false;
    nodsynth::runtime::WavData wav;
    std::string error;
    if (!nodsynth::runtime::readWav(wavPath, wav, error) || wav.channels < 2) return false;
    gAudio = std::move(wav.interleaved);
    gSampleRate = wav.sampleRate == 0 ? 48000 : wav.sampleRate;
    gPlaySample = 0;
    const auto devices = nodsynth::app::renderDevices();
    if (devices.empty() || !gOutput.start(devices.front().id, renderCallback, nullptr, error)) return false;
    gOutput.begin();
    gPlaying = true;
    SetTimer(window, 1, 30, nullptr);
    return true;
}

void openSong(HWND window)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"NodSynth song\0*.json;*.nodsong.json\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;
    std::string error;
    auto loaded = nodsynth::song::loadSong(file, error);
    if (!loaded) return;
    stopPlayback();
    gSong = std::move(*loaded);
    gPath = file;
    reloadNotes();
    refreshPanel();
    InvalidateRect(window, nullptr, TRUE);
}

void finishDrag(HWND window, int x, int y)
{
    nodsynth::persist::Json batch = nodsynth::persist::Json::object();
    batch.set("schemaVersion", nodsynth::persist::Json::number(1));
    nodsynth::persist::Json commands = nodsynth::persist::Json::array();
    if (gDragKind == DragKind::note && gDrag != nullptr) {
        const auto absolute = nodsynth::daw::tickAtPixel(x, gSong.ppq, pianoTime());
        const auto tick = absolute > gDrag->clipStart ? absolute - gDrag->clipStart : 0;
        commands.push(nodsynth::daw::moveNoteCommand(gDrag->trackId, gDrag->noteId, tick, nodsynth::daw::pitchAtPianoRow(y)));
    } else if ((gDragKind == DragKind::gain || gDragKind == DragKind::pan) && gDragStrip < gStrips.size()) {
        const auto strip = gStrips[gDragStrip];
        if (gDragKind == DragKind::gain) commands.push(nodsynth::daw::setGainCommand(strip.trackId, nodsynth::daw::gainAtFader(y - mixerTopFor(window), strip)));
        else commands.push(nodsynth::daw::setPanCommand(strip.trackId, nodsynth::daw::panAtStrip(x, strip)));
    }
    gDragKind = DragKind::none;
    gDrag = nullptr;
    if (commands.asArray().empty()) return;
    batch.set("commands", std::move(commands));
    if (!applyBatch(batch)) return;
    InvalidateRect(window, nullptr, TRUE);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT:
        paint(window);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == 1) openSong(window);
        if (LOWORD(wparam) == 2) {
            if (gPlaying) stopPlayback();
            else startPlayback(window);
        }
        if (LOWORD(wparam) == 3 && nodsynth::song::undoSong(gSong).ok) {
            reloadNotes();
            refreshPanel();
            InvalidateRect(window, nullptr, TRUE);
        }
        if (LOWORD(wparam) == 4 && gPanel != nullptr) {
            const int length = GetWindowTextLengthA(gPanel);
            std::string text(static_cast<std::size_t>(length) + 1, '\0');
            GetWindowTextA(gPanel, text.data(), length + 1);
            text.resize(std::strlen(text.c_str()));
            std::string error;
            auto batch = nodsynth::persist::Json::parse(text, error);
            if (batch && applyBatch(*batch)) InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (gSong.tracks.empty() || gSelected >= gSong.tracks.size()) return 0;
        const double next = std::clamp(gSong.tracks[gSelected].gain + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 0.05 : -0.05), 0.0, 2.0);
        nodsynth::persist::Json batch = nodsynth::persist::Json::object();
        batch.set("schemaVersion", nodsynth::persist::Json::number(1));
        nodsynth::persist::Json commands = nodsynth::persist::Json::array();
        commands.push(nodsynth::daw::setGainCommand(gSong.tracks[gSelected].id, next));
        batch.set("commands", std::move(commands));
        if (applyBatch(batch)) InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        layoutPanel(window);
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int mixerTop = mixerTopFor(window);
        gDrag = nullptr;
        gDragKind = DragKind::none;
        if (y >= mixerTop) {
            for (std::size_t index = 0; index < gStrips.size(); ++index) {
                const auto& strip = gStrips[index];
                if (x < strip.x || x >= strip.x + strip.width) continue;
                gSelected = index;
                gDragStrip = index;
                const int localY = y - mixerTop;
                gDragKind = localY >= strip.height - 24 ? DragKind::pan : DragKind::gain;
                reloadNotes();
                break;
            }
        } else {
            gDrag = nodsynth::daw::noteAt(gNotes, x, y);
            if (gDrag != nullptr) gDragKind = DragKind::note;
        }
        SetCapture(window);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONUP:
        finishDrag(window, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_SPACE) {
            if (gPlaying) stopPlayback();
            else startPlayback(window);
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        if (!gPlaying) KillTimer(window, 1);
        return 0;
    case WM_DESTROY:
        stopPlayback();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"NodSynthDaw";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, L"NodSynthDaw", L"NodSynth DAW", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640, nullptr, nullptr, instance, nullptr);
    HMENU menu = CreateMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open song");
    AppendMenuW(menu, MF_STRING, 2, L"Play / Stop");
    AppendMenuW(menu, MF_STRING, 3, L"Undo");
    AppendMenuW(menu, MF_STRING, 4, L"Apply panel");
    SetMenu(window, menu);
    gPanel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 100, kPanelHeight, window, nullptr, instance, nullptr);
    layoutPanel(window);
    refreshPanel();
    ShowWindow(window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
