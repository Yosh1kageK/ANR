#include "renderer/clips/video/mp4.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace renderer::clips::video {

FileReader::~FileReader() {
    if (handle_ == -1)
        return;
#ifdef _WIN32
    CloseHandle((HANDLE)handle_);
#else
    close((int)handle_);
#endif
}

bool FileReader::Open(const std::string& path, std::string* why) {
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wide > 0 ? wide : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "cannot open " + path;
        return false;
    }
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    size_ = uint64_t(size.QuadPart);
    handle_ = (intptr_t)h;
#else
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        *why = "cannot open " + path;
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        *why = "cannot stat " + path;
        return false;
    }
    size_ = uint64_t(st.st_size);
    handle_ = fd;
#endif
    return true;
}

bool FileReader::ReadAt(uint64_t offset, void* data, size_t size) const {
    if (handle_ == -1 || offset + size > size_)
        return false;
    uint8_t* out = static_cast<uint8_t*>(data);
    while (size > 0) {
#ifdef _WIN32
        OVERLAPPED overlapped = {};
        overlapped.Offset = DWORD(offset & 0xffffffffu);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        DWORD got = 0;
        if (!ReadFile((HANDLE)handle_, out, DWORD(std::min<size_t>(size, 1u << 30)), &got, &overlapped) || got == 0)
            return false;
#else
        const ssize_t got = pread((int)handle_, out, size, off_t(offset));
        if (got <= 0)
            return false;
#endif
        out += got;
        offset += uint64_t(got);
        size -= size_t(got);
    }
    return true;
}

namespace {

constexpr uint32_t FourCc(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) |
           uint32_t(uint8_t(s[3]));
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

struct Box {
    uint32_t type = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;  // of the payload
};

// Next box in [p, end); false at the end or on a damaged size.
bool NextBox(const uint8_t*& p, const uint8_t* end, Box* box) {
    if (end - p < 8)
        return false;
    uint64_t size = Be32(p);
    box->type = Be32(p + 4);
    size_t header = 8;
    if (size == 1) {
        if (end - p < 16)
            return false;
        size = Be64(p + 8);
        header = 16;
    } else if (size == 0) {
        size = uint64_t(end - p);
    }
    if (size < header || size > uint64_t(end - p))
        return false;
    box->payload = p + header;
    box->size = size_t(size - header);
    p += size;
    return true;
}

const Box* Find(const std::vector<Box>& boxes, uint32_t type) {
    for (const Box& b : boxes)
        if (b.type == type)
            return &b;
    return nullptr;
}

std::vector<Box> Children(const Box& parent, size_t skip = 0) {
    std::vector<Box> out;
    if (parent.size < skip)
        return out;
    const uint8_t* p = parent.payload + skip;
    const uint8_t* end = parent.payload + parent.size;
    Box b;
    while (NextBox(p, end, &b))
        out.push_back(b);
    return out;
}

struct Edit {
    uint64_t segment_duration;
    int64_t media_time;
    int32_t rate;  // 16.16
};

struct Parsed {
    Mp4Track track;
    uint32_t movie_timescale = 0;
    std::vector<Edit> edits;
    bool found = false;
    std::string reject;  // why a video track is unsupported
};

bool ParseSampleEntry(const Box& stsd, Mp4Track* track, std::string* why) {
    if (stsd.size < 16) {
        *why = "damaged stsd";
        return false;
    }
    const uint8_t* p = stsd.payload + 8;  // version and flags, entry count
    Box entry;
    const uint8_t* end = stsd.payload + stsd.size;
    if (!NextBox(p, end, &entry)) {
        *why = "damaged sample entry";
        return false;
    }
    const uint32_t format = entry.type;
    if (format == FourCc("av01"))
        track->codec = Codec::AV1;
    else if (format == FourCc("avc1") || format == FourCc("avc3"))
        track->codec = Codec::H264;
    else if (format == FourCc("hvc1") || format == FourCc("hev1"))
        track->codec = Codec::H265;
    else {
        char name[5] = {char(format >> 24), char(format >> 16), char(format >> 8), char(format), 0};
        *why = std::string("video codec '") + name + "' is not supported (AV1, H.264 and H.265 are)";
        return false;
    }
    if (entry.size < 78) {
        *why = "damaged visual sample entry";
        return false;
    }
    track->width = Be16(entry.payload + 24);
    track->height = Be16(entry.payload + 26);
    for (const Box& child : Children(entry, 78)) {
        if (child.type == FourCc("av1C") && track->codec == Codec::AV1)
            track->config.assign(child.payload, child.payload + child.size);
        else if (child.type == FourCc("avcC") && track->codec == Codec::H264) {
            track->config.assign(child.payload, child.payload + child.size);
            if (child.size >= 5)
                track->nal_length_size = (child.payload[4] & 3) + 1u;
        } else if (child.type == FourCc("hvcC") && track->codec == Codec::H265) {
            track->config.assign(child.payload, child.payload + child.size);
            if (child.size >= 22)
                track->nal_length_size = (child.payload[21] & 3) + 1u;
        } else if (child.type == FourCc("colr") && child.size >= 11 && Be32(child.payload) == FourCc("nclx")) {
            track->has_colr = true;
            track->color_primaries = uint8_t(Be16(child.payload + 4));
            track->transfer = uint8_t(Be16(child.payload + 6));
            track->matrix = uint8_t(Be16(child.payload + 8));
            track->full_range = (child.payload[10] & 0x80) != 0;
        }
    }
    if (track->config.empty()) {
        *why = "the sample entry has no codec configuration box";
        return false;
    }
    return true;
}

bool ParseTrak(const Box& trak, Parsed* parsed, std::string* why) {
    const std::vector<Box> top = Children(trak);
    const Box* mdia = Find(top, FourCc("mdia"));
    if (!mdia)
        return true;
    const std::vector<Box> media = Children(*mdia);
    const Box* hdlr = Find(media, FourCc("hdlr"));
    if (!hdlr || hdlr->size < 12 || Be32(hdlr->payload + 8) != FourCc("vide"))
        return true;  // not a video track
    if (parsed->found)
        return true;  // the first video track only
    const Box* mdhd = Find(media, FourCc("mdhd"));
    const Box* minf = Find(media, FourCc("minf"));
    if (!mdhd || !minf || mdhd->size < 24) {
        *why = "damaged video track";
        return false;
    }
    Mp4Track& t = parsed->track;
    t.timescale = mdhd->payload[0] == 1 ? (mdhd->size >= 32 ? Be32(mdhd->payload + 20) : 0) : Be32(mdhd->payload + 12);
    const std::vector<Box> minf_children = Children(*minf);
    const Box* stbl = Find(minf_children, FourCc("stbl"));
    if (!stbl || t.timescale == 0) {
        *why = "the video track has no sample table";
        return false;
    }
    const std::vector<Box> st = Children(*stbl);
    const Box* stsd = Find(st, FourCc("stsd"));
    if (!stsd) {
        *why = "the video track has no sample description";
        return false;
    }
    std::string entry_why;
    if (!ParseSampleEntry(*stsd, &t, &entry_why)) {
        parsed->reject = entry_why;  // another video track may be supported; the reason stays if none is
        return true;
    }
    const Box* stts = Find(st, FourCc("stts"));
    const Box* ctts = Find(st, FourCc("ctts"));
    const Box* stss = Find(st, FourCc("stss"));
    const Box* stsc = Find(st, FourCc("stsc"));
    const Box* stsz = Find(st, FourCc("stsz"));
    const Box* stco = Find(st, FourCc("stco"));
    const Box* co64 = Find(st, FourCc("co64"));
    if (!stts || !stsc || !stsz || (!stco && !co64) || stts->size < 8 || stsc->size < 8 || stsz->size < 12) {
        *why = "the sample table is incomplete (a fragmented MP4 is not supported)";
        return false;
    }
    const uint32_t sample_count = Be32(stsz->payload + 8);
    const uint32_t uniform_size = Be32(stsz->payload + 4);
    if (uniform_size == 0 && stsz->size < 12 + size_t(sample_count) * 4) {
        *why = "damaged stsz";
        return false;
    }
    t.samples.assign(sample_count, Sample{});
    for (uint32_t i = 0; i < sample_count; i++)
        t.samples[i].size = uniform_size ? uniform_size : Be32(stsz->payload + 12 + size_t(i) * 4);

    // dts
    {
        const uint32_t n = Be32(stts->payload + 4);
        if (stts->size < 8 + size_t(n) * 8) {
            *why = "damaged stts";
            return false;
        }
        uint64_t sample = 0;
        int64_t dts = 0;
        for (uint32_t i = 0; i < n && sample < sample_count; i++) {
            const uint32_t count = Be32(stts->payload + 8 + size_t(i) * 8);
            const uint32_t delta = Be32(stts->payload + 12 + size_t(i) * 8);
            for (uint32_t k = 0; k < count && sample < sample_count; k++) {
                t.samples[sample].dts = dts;
                t.samples[sample].pts = dts;
                dts += delta;
                sample++;
            }
        }
        if (sample != sample_count) {
            *why = "stts does not cover every sample";
            return false;
        }
    }
    // pts = dts + composition offset
    if (ctts && ctts->size >= 8) {
        const uint32_t n = Be32(ctts->payload + 4);
        if (ctts->size < 8 + size_t(n) * 8) {
            *why = "damaged ctts";
            return false;
        }
        uint64_t sample = 0;
        for (uint32_t i = 0; i < n && sample < sample_count; i++) {
            const uint32_t count = Be32(ctts->payload + 8 + size_t(i) * 8);
            const int32_t offset = int32_t(Be32(ctts->payload + 12 + size_t(i) * 8));
            for (uint32_t k = 0; k < count && sample < sample_count; k++)
                t.samples[sample++].pts += offset;
        }
    }
    // sync samples: all of them when stss is absent
    if (stss && stss->size >= 8) {
        const uint32_t n = Be32(stss->payload + 4);
        if (stss->size < 8 + size_t(n) * 4) {
            *why = "damaged stss";
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t number = Be32(stss->payload + 8 + size_t(i) * 4);
            if (number >= 1 && number <= sample_count)
                t.samples[number - 1].sync = true;
        }
    } else {
        for (Sample& s : t.samples)
            s.sync = true;
    }
    // offsets
    {
        std::vector<uint64_t> chunk_offsets;
        if (stco) {
            const uint32_t n = stco->size >= 8 ? Be32(stco->payload + 4) : 0;
            if (stco->size < 8 + size_t(n) * 4) {
                *why = "damaged stco";
                return false;
            }
            for (uint32_t i = 0; i < n; i++)
                chunk_offsets.push_back(Be32(stco->payload + 8 + size_t(i) * 4));
        } else {
            const uint32_t n = co64->size >= 8 ? Be32(co64->payload + 4) : 0;
            if (co64->size < 8 + size_t(n) * 8) {
                *why = "damaged co64";
                return false;
            }
            for (uint32_t i = 0; i < n; i++)
                chunk_offsets.push_back(Be64(co64->payload + 8 + size_t(i) * 8));
        }
        const uint32_t n = Be32(stsc->payload + 4);
        if (stsc->size < 8 + size_t(n) * 12) {
            *why = "damaged stsc";
            return false;
        }
        uint64_t sample = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t first_chunk = Be32(stsc->payload + 8 + size_t(i) * 12);
            const uint32_t per_chunk = Be32(stsc->payload + 12 + size_t(i) * 12);
            const uint32_t next_chunk = i + 1 < n ? Be32(stsc->payload + 8 + size_t(i + 1) * 12) : uint32_t(chunk_offsets.size()) + 1;
            for (uint32_t chunk = first_chunk; chunk < next_chunk && chunk >= 1 && chunk <= chunk_offsets.size(); chunk++) {
                uint64_t offset = chunk_offsets[chunk - 1];
                for (uint32_t k = 0; k < per_chunk && sample < sample_count; k++) {
                    t.samples[sample].offset = offset;
                    offset += t.samples[sample].size;
                    sample++;
                }
            }
        }
        if (sample != sample_count) {
            *why = "stsc and stco do not cover every sample";
            return false;
        }
    }
    // edit list
    if (const Box* edts = Find(top, FourCc("edts"))) {
        const std::vector<Box> edit_children = Children(*edts);
        if (const Box* elst = Find(edit_children, FourCc("elst")); elst && elst->size >= 8) {
            const uint8_t version = elst->payload[0];
            const uint32_t n = Be32(elst->payload + 4);
            const size_t entry_size = version == 1 ? 20 : 12;
            if (elst->size >= 8 + size_t(n) * entry_size) {
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t* e = elst->payload + 8 + size_t(i) * entry_size;
                    Edit edit;
                    if (version == 1) {
                        edit.segment_duration = Be64(e);
                        edit.media_time = int64_t(Be64(e + 8));
                        edit.rate = int32_t(Be32(e + 16));
                    } else {
                        edit.segment_duration = Be32(e);
                        edit.media_time = int32_t(Be32(e + 4));
                        edit.rate = int32_t(Be32(e + 8));
                    }
                    parsed->edits.push_back(edit);
                }
            }
        }
    }
    parsed->found = true;
    return true;
}

}  // namespace

std::unique_ptr<Mp4> Mp4::Open(const std::string& path, std::string* why) {
    std::string reason;
    if (!why)
        why = &reason;
    std::unique_ptr<Mp4> mp4(new Mp4());
    if (!mp4->file_.Open(path, why))
        return nullptr;
    // Top-level boxes: moov, and no moof.
    uint64_t pos = 0;
    uint64_t moov_offset = 0, moov_size = 0;
    bool fragmented = false, has_ftyp = false;
    while (pos + 8 <= mp4->file_.size()) {
        uint8_t header[16];
        const size_t header_read = size_t(std::min<uint64_t>(16, mp4->file_.size() - pos));
        if (!mp4->file_.ReadAt(pos, header, header_read))
            break;
        uint64_t size = Be32(header);
        const uint32_t type = Be32(header + 4);
        uint64_t header_size = 8;
        if (size == 1 && header_read >= 16) {
            size = Be64(header + 8);
            header_size = 16;
        } else if (size == 0) {
            size = mp4->file_.size() - pos;
        }
        if (size < header_size || pos + size > mp4->file_.size())
            break;
        if (type == FourCc("ftyp"))
            has_ftyp = true;
        else if (type == FourCc("moov")) {
            moov_offset = pos + header_size;
            moov_size = size - header_size;
        } else if (type == FourCc("moof"))
            fragmented = true;
        pos += size;
    }
    if (!has_ftyp && moov_size == 0) {
        *why = "not an ISO base media file (MP4 or MOV)";
        return nullptr;
    }
    if (moov_size == 0) {
        *why = "no moov box (the file is incomplete or not an MP4)";
        return nullptr;
    }
    if (fragmented) {
        *why = "fragmented MP4 is not supported";
        return nullptr;
    }
    if (moov_size > (512u << 20)) {
        *why = "moov box is too large";
        return nullptr;
    }
    std::vector<uint8_t> moov(moov_size);
    if (!mp4->file_.ReadAt(moov_offset, moov.data(), moov.size())) {
        *why = "cannot read the moov box";
        return nullptr;
    }
    Box moov_box;
    moov_box.payload = moov.data();
    moov_box.size = moov.size();
    Parsed parsed;
    for (const Box& child : Children(moov_box)) {
        if (child.type == FourCc("mvhd") && child.size >= 20)
            parsed.movie_timescale = child.payload[0] == 1 ? Be32(child.payload + 20) : Be32(child.payload + 12);
        else if (child.type == FourCc("trak") && !ParseTrak(child, &parsed, why))
            return nullptr;
    }
    if (!parsed.found) {
        *why = parsed.reject.empty() ? "no video track" : parsed.reject;
        return nullptr;
    }
    Mp4Track& t = parsed.track;
    if (t.samples.empty()) {
        *why = "the video track has no samples";
        return nullptr;
    }

    // Display window from the edit list: leading empty edits only delay the start; one media segment selects the
    // samples whose pts fall in it.
    int64_t window_start = INT64_MIN, window_end = INT64_MAX;
    {
        int media_edits = 0;
        for (const Edit& e : parsed.edits) {
            if (e.media_time < 0)
                continue;
            media_edits++;
            if (e.rate != 0x10000) {
                *why = "an edit list with a rate other than 1 is not supported";
                return nullptr;
            }
            window_start = e.media_time;
            if (e.segment_duration != 0 && parsed.movie_timescale != 0)
                window_end = window_start + int64_t(double(e.segment_duration) * t.timescale / parsed.movie_timescale + 0.5);
        }
        if (media_edits > 1) {
            *why = "an edit list with several media segments is not supported";
            return nullptr;
        }
    }
    std::vector<uint32_t> order;
    order.reserve(t.samples.size());
    for (uint32_t i = 0; i < t.samples.size(); i++)
        if (t.samples[i].pts >= window_start && t.samples[i].pts < window_end)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return t.samples[a].pts < t.samples[b].pts; });
    if (order.empty()) {
        *why = "the edit list shows no sample";
        return nullptr;
    }
    for (size_t i = 1; i < order.size(); i++)
        if (t.samples[order[i]].pts == t.samples[order[i - 1]].pts) {
            *why = "two samples have the same presentation time";
            return nullptr;
        }
    // Constant frame rate: the steps between display frames agree within a media tick.
    t.first_pts = t.samples[order.front()].pts;
    if (order.size() > 1) {
        const double mean = double(t.samples[order.back()].pts - t.first_pts) / double(order.size() - 1);
        for (size_t i = 1; i < order.size(); i++) {
            const double step = double(t.samples[order[i]].pts - t.samples[order[i - 1]].pts);
            if (std::fabs(step - mean) > 1.0 + 1e-9) {
                char text[160];
                std::snprintf(text, sizeof(text),
                              "variable frame rate (frame %zu is %.0f ticks after frame %zu, the mean is %.2f)", i,
                              step, i - 1, mean);
                *why = text;
                return nullptr;
            }
        }
        t.pts_step = int64_t(mean + 0.5);
        t.fps = double(t.timescale) / mean;
    } else {
        // One frame: the sample's duration from the next dts is unknown; assume 1/timescale per frame is wrong, use dts delta.
        t.pts_step = 1;
        t.fps = double(t.timescale);
    }
    t.display_to_sample = order;
    t.sample_to_display.assign(t.samples.size(), -1);
    for (size_t d = 0; d < order.size(); d++)
        t.sample_to_display[order[d]] = int64_t(d);
    mp4->track_ = std::move(parsed.track);
    return mp4;
}

bool Mp4::ReadSample(uint32_t index, std::vector<uint8_t>* out) const {
    if (index >= track_.samples.size())
        return false;
    const Sample& s = track_.samples[index];
    out->resize(s.size);
    return file_.ReadAt(s.offset, out->data(), s.size);
}

}  // namespace renderer::clips::video
