// A recording of a rendering feature's inputs (FG, SR or RR) into an nr-record folder (FORMAT.md), losslessly and
// without waits on the source's thread. Each real frame's textures are coded on the GPU (a Packer, on the source's
// command list) into a ring slot in device memory; a copy queue of the recorder copies each finished slot to a
// readback buffer; worker threads copy (or zstd-compress) its words in blocks into memory, which frees the slot; a
// writer thread appends the frames to frames.bin and frames.idx in order and each call's parameters to params.bin. A
// full ring drops the real frame and marks the gap. The source's queue only gets the pack.
//
//   auto r = record::Recorder::Start(device, packer, config, session, inputs, first_frame_inputs, error);
//   r->Frame(list, real_frame_number, inputs);   // each real frame, on the source's thread
//   r->Call(header, values);                      // each call's parameters (FG: each generated frame's)
//   if (std::string why = r->Due(); !why.empty()) r->Stop(why);
//   ... once r->finished(): r.reset()
//
// Windows (a D3D12 ring and copy queue, or a Ring of another API), natively or under Wine. The source's thread calls
// Frame, Call, Stop and Due.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "record/format.h"

namespace record {

constexpr uint32_t MAX_SLOTS = 16;

enum class Level { Info, Warn, Error };
using Log = std::function<void(Level, const std::string&)>;

// The GPU side: codes a real frame's textures into a slot buffer (the coding of FORMAT.md) on the source's command
// list, and tells when that has finished. Textures and lists are the packer's own handles; the recorder passes them
// through.
class Packer {
public:
    virtual ~Packer() = default;
    // A texture's coding; false (and why) if its format has none.
    virtual bool Describe(const void* texture, Channels& out, std::string& error) = 0;
    struct Texture {
        const void* texture = nullptr;
        const void* reference = nullptr;  // coded as the difference from this one (same format and size), or null
        uint32_t table = 0;               // its chunk table's first word in the slot
    };
    // Codes the textures into slot_buffer on the list: word 0 starts at data_start and allocates the chunks' words.
    // Then the used word count is kept for `slot` (< MAX_SLOTS). No waits.
    virtual bool Pack(void* list, uint32_t slot, ID3D12Resource* slot_buffer, uint32_t data_start,
                      const std::vector<Texture>& textures, std::string& error) = 0;
    // Any thread: 1 when slot's pack has finished on the GPU (its used bytes and GPU ms), 0 while it runs, -1 if the
    // slot has no pack. fence: a D3D12 fence and value that work reading the slot must wait for (else null).
    virtual int Done(uint32_t slot, uint64_t& bytes, double& gpu_ms, ID3D12Fence*& fence, uint64_t& fence_value) = 0;
    // Whether another device writes the slot buffers (D3D12_HEAP_FLAG_SHARED).
    virtual bool SharedSlots() const = 0;
};

// The ring's GPU side in one graphics API: the slot buffers, the packs into them on the source's command lists, and
// each finished slot in memory for the workers. Start with a D3D12 device and a Packer makes D3D12's (slots in device
// memory, copied on the recorder's copy queue); VulkanRing (vulkan_ring.h) is Vulkan's.
class Ring {
public:
    virtual ~Ring() = default;
    virtual bool Describe(const void* texture, Channels& out, std::string& error) = 0;
    // `slots` (up to MAX_SLOTS) slot buffers of `bytes` each, once per recording before its first Pack.
    virtual bool Create(unsigned slots, uint64_t bytes, std::string& error) = 0;
    // As Packer::Pack, into slot's buffer.
    virtual bool Pack(void* list, uint32_t slot, uint32_t data_start, const std::vector<Packer::Texture>& textures,
                      std::string& error) = 0;
    // As Packer::Done, without the fence.
    virtual int Done(uint32_t slot, uint64_t& bytes, double& gpu_ms) = 0;
    // The copier thread, once Done gave 1: the slot's first `bytes` in memory until Release(slot) (copied there, which
    // it waits for, or mapped), or null and why.
    virtual const uint8_t* Read(uint32_t slot, uint64_t bytes, std::string& error) = 0;
    virtual void Release(uint32_t slot) = 0;
};

// A texture role the source may give, in the order the slot codes them (FORMAT.md lists the roles per feature).
struct InputDesc {
    std::string role;
    bool required = false;   // no recording without it
    std::string reference;   // coded as its difference from this role's texture where that has the same format and size
};

// One role's texture in a real frame. A change of presence, size or format against the first frame stops the recording.
struct Input {
    const void* texture = nullptr;  // the packer's handle; null: not given
    uint32_t api_format = 0, width = 0, height = 0;
};

// What record.json says about the source.
struct Session {
    std::string feature;           // fg, sr or rr
    std::string source;            // the interface the inputs came through, e.g. ngx-dlssg
    std::string graphics_api;      // d3d12 or vulkan
    std::string recorder;          // the recording program and its version
    std::string game;              // the game's executable (its name without .exe ends the folder's name)
    uint32_t width = 0, height = 0;  // the feature's output size
    // The feature's own fields as JSON values (FG: multiplier, encoding, hdr, mode).
    std::vector<std::pair<std::string, std::string>> fields;
    std::string parameter_source;  // whose names the parameters and matrices are (ngx)
    std::vector<std::string> parameters, matrices;
};

struct Config {
    std::string root;            // recordings go into <root>/<date>-<time>-<game>[-<tag>]; empty: DefaultRoot("")
    unsigned ring = 4;           // slots, 2 to MAX_SLOTS
    int zstd_level = 0;          // the blocks' zstd level; 0: stored as coded
    unsigned threads = 0;        // workers; 0: a quarter of the logical cores, 2 to 8
    unsigned queue_mb = 1024;    // memory for frames that wait for the disk
    double free_gb = 50;         // stops below this free space
    unsigned max_seconds = 300;  // stops after this long; 0: no limit
    unsigned max_frames = 0;     // stops after this many real frames; 0: no limit
    std::string tag;             // ends the folder's name (<date>-<time>-<game>-<tag>), e.g. capture
    Log log;
};

// The folder recordings go under: `configured` (a Windows path, or under Wine a Unix path), else under Wine
// $XDG_DATA_HOME/nr-record or ~/.local/share/nr-record (as Z: paths), else %LOCALAPPDATA%\nr-record.
std::string DefaultRoot(const std::string& configured);

struct State;

class Recorder {
public:
    // A recording from this real frame: the textures' coding, the folder and its files, the ring on the device and the
    // threads. first: the inputs of the frame it starts at (InputDesc order). null and error if it cannot start.
    static std::unique_ptr<Recorder> Start(ID3D12Device* device, std::shared_ptr<Packer> packer, const Config& config,
                                           const Session& session, const std::vector<InputDesc>& inputs,
                                           const std::vector<Input>& first, std::string& error);
    // The same on another graphics API's ring.
    static std::unique_ptr<Recorder> Start(std::shared_ptr<Ring> ring, const Config& config, const Session& session,
                                           const std::vector<InputDesc>& inputs, const std::vector<Input>& first,
                                           std::string& error);
    ~Recorder();

    // Each real frame: its inputs (InputDesc order) into the next ring slot, or a dropped frame if the ring is full.
    // Nothing once stopping.
    void Frame(void* list, uint64_t frame_number, const std::vector<Input>& inputs);
    // Each call's parameters: values are CallBytes(parameters, matrices) - sizeof(CallHeader) bytes.
    void Call(const CallHeader& header, const uint8_t* values);
    // Why it should stop now (its time or frame limit, free space, a failed write), or empty.
    std::string Due() const;
    // Stops: the threads write what was packed, then record.json.
    void Stop(const std::string& reason);
    // Stop and waits up to `ms` for the threads.
    bool StopAndWait(const std::string& reason, unsigned ms);
    bool stopping() const;
    bool finished() const;  // the writer is done: the folder is complete

    const std::string& dir() const;
    const std::string& reason() const;
    std::string Layout() const;    // the textures: role, size, format, reference
    std::string Status() const;    // seconds, frames, GB, dropped so far
    std::string Progress() const;  // Status, MB/s and the pack and copy medians (the periodic log line)
    // After a stop: seconds, frames, GB, dropped with their cause, and the folder's name.
    std::string Summary(bool with_reason) const;
    // The final log line's numbers: frames, dropped with cause, lost, GB.
    std::string Totals() const;
    unsigned slots() const;
    double slot_mib() const;
    unsigned threads() const;
    int zstd_level() const;

private:
    std::shared_ptr<State> s_;
};

}  // namespace record
