#include "renderer/clips/captures/sources.h"

#include <filesystem>

#include "renderer/clips/captures/recording.h"

namespace renderer::clips::captures {
namespace fs = std::filesystem;

const Source* FindSource(const std::vector<Source>& sources, const std::string& id) {
    for (const Source& s : sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

std::vector<Source> BuildSources(const CaptureInfo& c, const PairSpec& pair, const std::vector<GeneratedFrame>& generated) {
    std::vector<Source> list;
    const bool recorded = c.format == RECORDING_FORMAT;
    // Where a texture is: a recording's frame and role, else the capture folder's file
    auto locate = [&](Source& s, int frame, const std::string& name) {
        s.recorded = recorded;
        s.role = name;
        s.path = recorded ? c.folder : c.folder + "/" + TextureFile(frame, name);
    };
    auto add_frame = [&](int frame, const std::string& label) {
        const FrameInfo* f = c.Frame(frame);
        if (!f || !f->files.count("backbuffer"))
            return;
        Source s;
        s.id = "real:" + std::to_string(frame);
        s.label = label;
        s.kind = SourceKind::REAL_FRAME;
        locate(s, frame, "backbuffer");
        s.info = f->files.at("backbuffer");
        s.coding = c.coding;
        s.frame = frame;
        list.push_back(s);
    };
    add_frame(pair.a, "Real frame " + std::to_string(pair.a));
    add_frame(pair.b, "Real frame " + std::to_string(pair.b));
    if (pair.kind == PairKind::REAL) {
        const FrameInfo* f = c.Frame(pair.b);
        if (f)
            for (const CallInfo& call : f->calls)
                if (call.index == pair.index && call.has_generated) {
                    Source s;
                    s.id = "ingame";
                    s.label = "In game (as captured)";
                    s.kind = SourceKind::IN_GAME;
                    s.path = c.folder + "/" + GeneratedFile(pair.b, call.index);
                    s.info = call.generated;
                    s.coding = c.coding;
                    s.frame = pair.b;
                    list.push_back(s);
                }
    } else {
        add_frame(pair.rendered, "Rendered (approx.)");
        for (Source& s : list)
            if (s.id == "real:" + std::to_string(pair.rendered)) {
                s.id = "rendered";
                s.kind = SourceKind::RENDERED;
            }
    }
    for (const GeneratedFrame& g : generated) {
        Source s;
        s.id = "gen:" + g.generator.id;
        s.label = g.generator.label;
        s.kind = SourceKind::GENERATED;
        s.path = g.file;
        s.raw = true;
        s.coding = c.coding;
        s.generator_id = g.generator.id;
        s.stale = g.stale;
        s.apex = g.generator.kind == Generator::APEX;
        s.dlss = g.generator.kind == Generator::DLSSG;
        if (g.stale)
            s.label += " (stale)";
        list.push_back(s);
    }
    // False colour: the depth of the pair's frames and the motion of the later one
    for (int frame : {pair.a, pair.b}) {
        const FrameInfo* f = c.Frame(frame);
        if (!f)
            continue;
        for (const char* name : {"depth", "motion"}) {
            auto it = f->files.find(name);
            if (it == f->files.end() || (std::string(name) == "motion" && frame == pair.a && pair.kind == PairKind::REAL))
                continue;
            Source s;
            s.id = std::string(name) + ":" + std::to_string(frame);
            s.label = std::string(name[0] == 'd' ? "Depth" : "Motion") + " of frame " + std::to_string(frame);
            s.kind = name[0] == 'd' ? SourceKind::DEPTH : SourceKind::MOTION;
            locate(s, frame, name);
            s.info = it->second;
            s.coding = Coding::VIZ;
            s.frame = frame;
            list.push_back(s);
        }
    }
    return list;
}

std::pair<std::string, std::string> DefaultPick(const std::vector<Source>& sources) {
    std::string a, b;
    auto first = [&](auto&& wanted) {
        for (const Source& s : sources)
            if (wanted(s))
                return s.id;
        return std::string();
    };
    a = first([](const Source& s) { return s.apex && !s.stale; });
    if (a.empty())
        a = first([](const Source& s) { return s.kind == SourceKind::IN_GAME; });
    if (a.empty())
        a = first([](const Source& s) { return s.kind == SourceKind::REAL_FRAME; });
    b = first([](const Source& s) { return s.dlss && !s.stale; });
    if (b.empty())
        b = first([](const Source& s) { return s.kind == SourceKind::RENDERED; });
    if (b.empty())
        for (const Source& s : sources)
            if (s.kind == SourceKind::REAL_FRAME && s.id != a)
                b = s.id;
    if (b.empty())
        b = a;
    return {a, b};
}

}  // namespace renderer::clips::captures
