#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace nodsynth::runtime {
struct WavData {
    std::uint32_t sampleRate{0};
    std::uint32_t channels{0};
    std::vector<float> interleaved;
};

inline void writeLe16(std::ostream& out, std::uint16_t value) {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8)};
    out.write(reinterpret_cast<const char*>(bytes), 2);
}

inline void writeLe32(std::ostream& out, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

inline bool writeWav(const std::filesystem::path& path, const WavData& wav, std::string& error) {
    if (wav.channels == 0 || wav.sampleRate == 0 || wav.interleaved.size() % wav.channels != 0) {
        error = "invalid WAV description";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open WAV file";
        return false;
    }
    const auto dataBytes = static_cast<std::uint32_t>(wav.interleaved.size() * sizeof(float));
    out.write("RIFF", 4);
    writeLe32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 3);
    writeLe16(out, static_cast<std::uint16_t>(wav.channels));
    writeLe32(out, wav.sampleRate);
    writeLe32(out, wav.sampleRate * wav.channels * sizeof(float));
    writeLe16(out, static_cast<std::uint16_t>(wav.channels * sizeof(float)));
    writeLe16(out, 32);
    out.write("data", 4);
    writeLe32(out, dataBytes);
    out.write(reinterpret_cast<const char*>(wav.interleaved.data()), static_cast<std::streamsize>(dataBytes));
    if (!out) {
        error = "failed to write WAV file";
        return false;
    }
    return true;
}

inline std::uint32_t readLe32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline bool readWav(const std::filesystem::path& path, WavData& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open WAV file";
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44) {
        error = "WAV file is truncated";
        return false;
    }
    wav.channels = bytes[22] | (static_cast<std::uint32_t>(bytes[23]) << 8);
    wav.sampleRate = readLe32(bytes.data() + 24);
    const auto dataBytes = readLe32(bytes.data() + 40);
    if (std::string(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF" || dataBytes > bytes.size() - 44) {
        error = "WAV file is invalid";
        return false;
    }
    const auto frames = dataBytes / sizeof(float) / std::max<std::uint32_t>(wav.channels, 1);
    wav.interleaved.resize(static_cast<std::size_t>(frames) * wav.channels);
    std::memcpy(wav.interleaved.data(), bytes.data() + 44, wav.interleaved.size() * sizeof(float));
    return true;
}
} // namespace nodsynth::runtime
