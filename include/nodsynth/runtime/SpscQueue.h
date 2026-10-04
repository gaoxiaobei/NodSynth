#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nodsynth::runtime {
template <typename T, std::uint32_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    bool push(const T& item) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) & mask;
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        item = slots_[read];
        read_.store((read + 1) & mask, std::memory_order_release);
        return true;
    }

    void discardAll() noexcept {
        auto item = T{};
        while (pop(item)) {}
    }

private:
    static constexpr std::uint32_t mask = Capacity - 1;
    std::array<T, Capacity> slots_{};
    std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint32_t> read_{0};
};
} // namespace nodsynth::runtime
