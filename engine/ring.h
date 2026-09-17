// ring.h - single-producer/single-consumer lock-free ring of 8ch float frames.
// Capture thread writes, render thread reads. Overflow drops the OLDEST frames
// (bounds latency); underrun is handled by the reader (returns short count).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sr {

class RingBuffer {
public:
    static constexpr int kChannels = 8;

    // capacityFrames is rounded up to a power of two.
    explicit RingBuffer(size_t capacityFrames) {
        size_t cap = 256;
        while (cap < capacityFrames) cap <<= 1;
        mask_ = cap - 1;
        buf_.resize(cap * kChannels, 0.0f);
        read_.store(0);
        write_.store(0);
        overruns_.store(0);
    }

    size_t Capacity() const { return mask_ + 1; }
    size_t Available() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
    uint64_t Overruns() const { return overruns_.load(std::memory_order_relaxed); }

    void Reset() {
        read_.store(0, std::memory_order_release);
        write_.store(0, std::memory_order_release);
        overruns_.store(0, std::memory_order_relaxed);
    }

    // Producer. Returns frames written (always == frames; oldest are dropped on overflow).
    size_t Write(const float* src, size_t frames) {
        uint64_t w = write_.load(std::memory_order_relaxed);
        uint64_t r = read_.load(std::memory_order_acquire);
        uint64_t cap = mask_ + 1;
        if (w - r + frames > cap) {
            uint64_t drop = w - r + frames - cap;
            r += drop;
            read_.store(r, std::memory_order_release);
            overruns_.fetch_add(drop, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < frames; ++i) {
            size_t pos = static_cast<size_t>(w + i) & mask_;
            std::memcpy(&buf_[pos * kChannels], src + i * kChannels, sizeof(float) * kChannels);
        }
        write_.store(w + frames, std::memory_order_release);
        return frames;
    }

    // Consumer. Returns frames actually read; dst must hold `frames` frames.
    size_t Read(float* dst, size_t frames) {
        uint64_t r = read_.load(std::memory_order_relaxed);
        uint64_t w = write_.load(std::memory_order_acquire);
        size_t avail = static_cast<size_t>(w - r);
        size_t n = (frames < avail) ? frames : avail;
        for (size_t i = 0; i < n; ++i) {
            size_t pos = static_cast<size_t>(r + i) & mask_;
            std::memcpy(dst + i * kChannels, &buf_[pos * kChannels], sizeof(float) * kChannels);
        }
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<uint64_t> read_{0};
    std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> overruns_{0};
};

} // namespace sr
