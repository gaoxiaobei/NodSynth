#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
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
    std::uint16_t encoding{3};
    std::uint16_t bitsPerSample{32};
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

inline bool readWavHeader(std::istream& in, WavData& wav, std::uint32_t& dataBytes, std::string& error) {
    in.seekg(0, std::ios::end);
    const auto fileSize = in.tellg();
    in.seekg(0);
    unsigned char header[12]; in.read(reinterpret_cast<char*>(header), 12);
    if (!in || std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) {
        error = "WAV RIFF header is invalid or truncated"; return false;
    }
    const auto riffEnd = static_cast<std::uint64_t>(readLe32(header + 4)) + 8;
    if (fileSize < 12 || riffEnd < 12 || riffEnd > static_cast<std::uint64_t>(fileSize)) {
        error = "WAV RIFF size exceeds file length"; return false;
    }
    bool haveFormat = false;
    while (static_cast<std::uint64_t>(in.tellg()) + 8 <= riffEnd) {
        unsigned char chunk[8]; in.read(reinterpret_cast<char*>(chunk), 8);
        const auto size = readLe32(chunk + 4);
        const auto begin = static_cast<std::uint64_t>(in.tellg());
        if (!in || begin + size + (size & 1u) > riffEnd) { error = "WAV chunk is truncated"; return false; }
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || haveFormat) { error = "WAV format chunk is invalid"; return false; }
            unsigned char format[16]; in.read(reinterpret_cast<char*>(format), 16);
            wav.encoding = format[0] | (static_cast<std::uint16_t>(format[1]) << 8);
            wav.channels = format[2] | (static_cast<std::uint32_t>(format[3]) << 8);
            wav.sampleRate = readLe32(format + 4);
            wav.bitsPerSample = format[14] | (static_cast<std::uint16_t>(format[15]) << 8);
            if(wav.encoding==0xfffe) {
                if(size<40) {error="WAV extensible format is truncated";return false;}
                unsigned char extension[24];in.read(reinterpret_cast<char*>(extension),24);
                const unsigned char guidTail[12]{0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                const auto extensionBytes=extension[0]|(static_cast<std::uint16_t>(extension[1])<<8);
                const auto validBits=extension[2]|(static_cast<std::uint16_t>(extension[3])<<8);
                const auto subtype=readLe32(extension+8);
                if(extensionBytes<22 || validBits==0 || validBits>wav.bitsPerSample || std::memcmp(extension+12,guidTail,12) || (subtype!=1 && subtype!=3)) {
                    error="WAV extensible subtype or valid bits is unsupported";return false;
                }
                wav.encoding=static_cast<std::uint16_t>(subtype);
            }
            const auto alignment = format[12] | (static_cast<std::uint32_t>(format[13]) << 8);
            if ((! (wav.encoding == 3 && wav.bitsPerSample == 32) &&
                 ! (wav.encoding == 1 && (wav.bitsPerSample == 16 || wav.bitsPerSample == 24 || wav.bitsPerSample == 32))) ||
                wav.channels == 0 || wav.sampleRate == 0 || alignment != wav.channels * (wav.bitsPerSample / 8)) {
                error = "WAV must have valid float32 or PCM16/24/32 format"; return false;
            }
            haveFormat = true;
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (!haveFormat || size % (wav.channels * (wav.bitsPerSample / 8))) { error = "WAV data format or size is invalid"; return false; }
            dataBytes = size; return true;
        }
        in.seekg(static_cast<std::streamoff>(begin + size + (size & 1u)));
    }
    error = "WAV is missing its format or data chunk"; return false;
}

inline bool readWav(const std::filesystem::path& path, WavData& wav, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open WAV file";
        return false;
    }
    std::uint32_t dataBytes = 0;
    if (!readWavHeader(in, wav, dataBytes, error)) return false;
    const auto sampleBytes = wav.bitsPerSample / 8;
    if (dataBytes % (sampleBytes * wav.channels)) { error = "invalid WAV data size"; return false; }
    const auto frames = dataBytes / sampleBytes / wav.channels;
    wav.interleaved.resize(static_cast<std::size_t>(frames) * wav.channels);
    if (wav.encoding == 1) {
        for (auto& value : wav.interleaved) {
            unsigned char bytes[4]{}; in.read(reinterpret_cast<char*>(bytes), sampleBytes);
            if (!in) { error = "WAV file is truncated"; return false; }
            if (sampleBytes == 2) value = static_cast<float>(static_cast<std::int16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8))) / 32768.f;
            else if (sampleBytes == 3) {
                std::int32_t sample = bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) | (static_cast<std::uint32_t>(bytes[2]) << 16);
                if (sample & 0x800000) sample -= 0x1000000;
                value = static_cast<float>(sample / 8388608.0);
            } else value = static_cast<float>(static_cast<std::int32_t>(readLe32(bytes)) / 2147483648.0);
        }
        return true;
    }
    in.read(reinterpret_cast<char*>(wav.interleaved.data()), static_cast<std::streamsize>(wav.interleaved.size() * sizeof(float)));
    if (!in || static_cast<std::size_t>(in.gcount()) != wav.interleaved.size() * sizeof(float)) {
        error = "WAV file is truncated";
        return false;
    }
    return true;
}

// Reads [startFrame, startFrame + frameCount) from a float32 PCM WAV written by this module.
inline bool readWavRange(
    const std::filesystem::path& path,
    WavData& wav,
    std::uint64_t startFrame,
    std::uint64_t frameCount,
    std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open WAV file";
        return false;
    }
    std::uint32_t dataBytes = 0;
    if (!readWavHeader(in, wav, dataBytes, error)) return false;
    if (wav.encoding != 3) { error = "cached WAV range requires float32"; return false; }
    const auto totalFrames = static_cast<std::uint64_t>(dataBytes / sizeof(float) / wav.channels);
    if (startFrame > totalFrames) {
        error = "WAV range starts past the end";
        return false;
    }
    const auto available = totalFrames - startFrame;
    const auto frames = std::min(frameCount, available);
    wav.interleaved.assign(static_cast<std::size_t>(frames) * wav.channels, 0.f);
    if (frames == 0) return true;
    const auto byteOffset = static_cast<std::uint64_t>(in.tellg()) + startFrame * wav.channels * sizeof(float);
    in.seekg(static_cast<std::streamoff>(byteOffset));
    if (!in) {
        error = "failed to seek WAV file";
        return false;
    }
    in.read(reinterpret_cast<char*>(wav.interleaved.data()), static_cast<std::streamsize>(wav.interleaved.size() * sizeof(float)));
    if (!in || static_cast<std::size_t>(in.gcount()) != wav.interleaved.size() * sizeof(float)) {
        error = "WAV file is truncated";
        return false;
    }
    return true;
}

class WavStream {
public:
    ~WavStream() {
        if (open_) abort();
    }

    WavStream() = default;
    WavStream(const WavStream&) = delete;
    WavStream& operator=(const WavStream&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path, std::uint32_t sampleRate, std::uint32_t channels, std::string& error) {
        if (channels == 0 || channels > 65535 || sampleRate == 0) {
            error = "invalid WAV description";
            return false;
        }
        final_ = path;
        temporary_ = path;
        temporary_ += ".partial";
        output_.open(temporary_, std::ios::binary | std::ios::trunc);
        if (!output_) {
            error = "failed to open WAV file";
            return false;
        }
        channels_ = channels;
        output_.write("RIFF", 4);
        writeLe32(output_, 0);
        output_.write("WAVE", 4);
        output_.write("fmt ", 4);
        writeLe32(output_, 16);
        writeLe16(output_, 3);
        writeLe16(output_, static_cast<std::uint16_t>(channels));
        writeLe32(output_, sampleRate);
        writeLe32(output_, sampleRate * channels * static_cast<std::uint32_t>(sizeof(float)));
        writeLe16(output_, static_cast<std::uint16_t>(channels * sizeof(float)));
        writeLe16(output_, 32);
        output_.write("data", 4);
        writeLe32(output_, 0);
        open_ = static_cast<bool>(output_);
        if (!open_) error = "failed to write WAV file";
        return open_;
    }

    [[nodiscard]] bool write(const float* interleaved, std::size_t values, std::string& error) {
        const auto bytes = static_cast<std::uint64_t>(values) * sizeof(float);
        if (!open_ || interleaved == nullptr || values % channels_ != 0 || dataBytes_ > kRiffLimit || bytes > kRiffLimit - dataBytes_) {
            error = open_ ? "WAV exceeds the RIFF size limit" : "failed to write WAV file";
            abort();
            return false;
        }
        output_.write(reinterpret_cast<const char*>(interleaved), static_cast<std::streamsize>(bytes));
        if (!output_) {
            error = "failed to write WAV file";
            abort();
            return false;
        }
        dataBytes_ += bytes;
        return true;
    }

    [[nodiscard]] bool commit(std::string& error) {
        if (!open_ || dataBytes_ > kRiffLimit) {
            error = open_ ? "WAV exceeds the RIFF size limit" : "failed to write WAV file";
            abort();
            return false;
        }
        const auto size = static_cast<std::uint32_t>(dataBytes_);
        output_.seekp(4);
        writeLe32(output_, 36 + size);
        output_.seekp(40);
        writeLe32(output_, size);
        output_.flush();
        if (!output_) {
            error = "failed to write WAV file";
            abort();
            return false;
        }
        output_.close();
        open_ = false;
        std::error_code ignored;
        std::filesystem::remove(final_, ignored);
        std::error_code failure;
        std::filesystem::rename(temporary_, final_, failure);
        if (failure) {
            error = "failed to commit WAV file";
            std::filesystem::remove(temporary_, ignored);
            return false;
        }
        return true;
    }

    void abort() noexcept {
        if (output_.is_open()) output_.close();
        open_ = false;
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }

    [[nodiscard]] std::uint64_t dataBytes() const noexcept { return dataBytes_; }

private:
    static constexpr std::uint64_t kRiffLimit = 0xffffffffu - 36u;
    std::ofstream output_;
    std::filesystem::path final_;
    std::filesystem::path temporary_;
    std::uint64_t dataBytes_{0};
    std::uint32_t channels_{0};
    bool open_{false};
};
} // namespace nodsynth::runtime
