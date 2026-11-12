// What nr-replay's frame generation and SR replays share: a bounded queue between threads, the frame stream's writing
// end, recorded formats in D3D12, and the video rate at the recorded pace. Windows.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "record/reader.h"

namespace replay {

// A bounded queue between threads. Pop is false once it is closed and empty.
template <typename T>
class Channel {
public:
    explicit Channel(size_t capacity) : capacity_(capacity) {}
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_)
            return false;
        items_.push_back(std::move(value));
        changed_.notify_all();
        return true;
    }
    bool Pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty())
            return false;
        value = std::move(items_.front());
        items_.pop_front();
        changed_.notify_all();
        return true;
    }
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> items_;
    size_t capacity_;
    bool closed_ = false;
};

// The DXGI format of a recorded texture: D3D12 recordings give it; Vulkan recordings' formats map to their equivalents.
inline DXGI_FORMAT DxgiOf(const record::Recording& rec, const record::Texture& t) {
    if (rec.graphics_api != "vulkan")
        return (DXGI_FORMAT)t.api_format;
    switch (t.vk) {
    case 37: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 43: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 44: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 50: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 9: return DXGI_FORMAT_R8_UNORM;
    case 64: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 76: return DXGI_FORMAT_R16_FLOAT;
    case 122: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 83: return DXGI_FORMAT_R16G16_FLOAT;
    case 97: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 100: return DXGI_FORMAT_R32_FLOAT;
    case 103: return DXGI_FORMAT_R32G32_FLOAT;
    case 126: return DXGI_FORMAT_D32_FLOAT;
    case 130: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

inline bool DepthFamily(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D32_FLOAT ||
           f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D16_UNORM;
}

// The writer's end of the frame stream: a file or FIFO, or standard output.
class StreamWriter {
public:
    ~StreamWriter() {
        if (handle_ != INVALID_HANDLE_VALUE && own_)
            CloseHandle(handle_);
    }
    bool Open(const std::string& path, std::string& error) {
        if (path == "-") {
            // The stream keeps its own handle of standard output; what else prints there (a generator's DLL) goes to
            // standard error, out of the stream.
            if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &handle_,
                                 0, FALSE, DUPLICATE_SAME_ACCESS))
                handle_ = INVALID_HANDLE_VALUE;
            own_ = true;
            std::fflush(stdout);
            _dup2(_fileno(stderr), _fileno(stdout));
            SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
        } else {
            handle_ = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            own_ = true;
            if (handle_ != INVALID_HANDLE_VALUE && GetFileType(handle_) == FILE_TYPE_DISK)
                SetEndOfFile(handle_);  // a regular file starts empty; a FIFO or pipe has no end to set
        }
        if (handle_ == INVALID_HANDLE_VALUE || !handle_) {
            error = "cannot open the stream " + path + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }
    bool Write(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        while (n) {
            DWORD written = 0;
            const DWORD chunk = (DWORD)std::min<size_t>(n, 16u << 20);
            if (!WriteFile(handle_, p, chunk, &written, nullptr) || !written)
                return false;
            p += written;
            n -= written;
        }
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool own_ = false;
};

// The video rate at the recorded pace: the median interval between consecutive replayed real frames with nothing
// dropped between them, times the output frames per real frame; whole within 2% is taken whole.
inline double FrameRate(const record::Recording& rec, size_t first, size_t last, uint32_t per_real) {
    const double frequency = rec.meta.value("qpc_frequency", 0.0);
    std::vector<int64_t> intervals;
    for (size_t k = first + 1; k < last; k++)
        if (!rec.index[k].dropped && rec.index[k].qpc > rec.index[k - 1].qpc)
            intervals.push_back(rec.index[k].qpc - rec.index[k - 1].qpc);
    if (intervals.empty() || frequency <= 0)
        return 0;
    std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2, intervals.end());
    const double fps = frequency / (double)intervals[intervals.size() / 2] * per_real;
    return std::abs(fps - std::round(fps)) <= 0.02 * fps ? std::round(fps) : fps;
}

}  // namespace replay
