// Most-significant-bit-first reader for the bit syntax of AV1 (f(n), su(n), ns(n), leb128), H.264 and H.265 (u(n),
// ue(v), se(v)). A read past the end returns zeros and sets overrun(); callers check it once after a syntax
// structure instead of after each field.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace renderer::clips::video {

class BitReader {
public:
    BitReader() = default;
    BitReader(const uint8_t* data, size_t size) : data_(data), size_bits_(uint64_t(size) * 8) {}

    uint32_t Bits(int n) {  // n in 0..32
        uint64_t v = 0;
        for (int i = 0; i < n; i++)
            v = (v << 1) | Bit();
        return uint32_t(v);
    }
    uint32_t Bit() {
        if (pos_ >= size_bits_) {
            overrun_ = true;
            return 0;
        }
        const uint32_t b = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        pos_++;
        return b;
    }
    bool Flag() { return Bit() != 0; }
    void Skip(uint64_t n) {
        pos_ += n;
        if (pos_ > size_bits_) {
            overrun_ = true;
            pos_ = size_bits_;
        }
    }
    // AV1 su(n): n-bit two's complement.
    int32_t Signed(int n) {
        const int32_t value = int32_t(Bits(n));
        const int32_t sign_mask = 1 << (n - 1);
        return (value & sign_mask) ? value - 2 * sign_mask : value;
    }
    // AV1 ns(n): non-symmetric unsigned in [0, n).
    uint32_t NonSymmetric(uint32_t n) {
        int w = 0;
        for (uint32_t x = n; x != 0; x >>= 1)
            w++;
        const uint32_t m = (1u << w) - n;
        const uint32_t v = Bits(w - 1);
        if (v < m)
            return v;
        return (v << 1) - m + Bit();
    }
    // H.264 / H.265 ue(v) and se(v).
    uint32_t Ue() {
        int zeros = 0;
        while (!Bit() && !overrun_ && zeros < 32)
            zeros++;
        if (zeros >= 32) {
            overrun_ = true;
            return 0;
        }
        return zeros ? ((1u << zeros) - 1 + Bits(zeros)) : 0;
    }
    int32_t Se() {
        const uint32_t k = Ue();
        return (k & 1) ? int32_t((k + 1) >> 1) : -int32_t(k >> 1);
    }
    bool ByteAligned() const { return (pos_ & 7) == 0; }
    void ByteAlign() { pos_ = (pos_ + 7) & ~uint64_t(7); }
    uint64_t position() const { return pos_; }  // in bits
    size_t byte_position() const { return size_t(pos_ >> 3); }
    uint64_t remaining() const { return size_bits_ > pos_ ? size_bits_ - pos_ : 0; }
    bool overrun() const { return overrun_; }
    // H.264 / H.265 more_rbsp_data(): true if bits remain before the rbsp_trailing_bits.
    bool MoreRbspData() const {
        if (pos_ >= size_bits_)
            return false;
        // Find the last 1 bit in the data: the stop bit.
        size_t last = size_bits_ / 8;
        while (last > 0 && data_[last - 1] == 0)
            last--;
        if (last == 0)
            return false;
        const uint8_t b = data_[last - 1];
        int tz = 0;
        while (((b >> tz) & 1) == 0)
            tz++;
        const uint64_t stop_pos = uint64_t(last - 1) * 8 + (7 - tz);
        return pos_ < stop_pos;
    }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_bits_ = 0;
    uint64_t pos_ = 0;
    bool overrun_ = false;
};

// H.264 / H.265: the RBSP of a NAL unit payload (emulation prevention bytes, 0x03 after 00 00, removed).
inline std::vector<uint8_t> Unescape(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; i++) {
        const uint8_t b = data[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

}  // namespace renderer::clips::video
