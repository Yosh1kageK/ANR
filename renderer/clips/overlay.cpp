#include "renderer/clips/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace renderer::clips {

ImVec2 DrawPill(ImDrawList* list, ImFont* font, float size, ImVec2 position, const std::string& text, bool right_align) {
    const float pad = std::floor(size / 2.0f);
    const ImVec2 text_size = font->CalcTextSizeA(size, 1e9f, 0.0f, text.c_str());
    const float width = text_size.x + 2.0f * pad, height = size + pad;
    const float x0 = right_align ? position.x - width : position.x;
    list->AddRectFilled(ImVec2(x0, position.y), ImVec2(x0 + width, position.y + height), IM_COL32(0, 0, 0, 170),
                        std::floor(pad / 2.0f) + 2.0f);
    list->AddText(font, size, ImVec2(x0 + pad, position.y + std::floor(pad / 2.0f) - 1.0f + (height - size - pad) * 0.5f),
                  IM_COL32(255, 255, 255, 255), text.c_str());
    return ImVec2(width, height);
}

ImVec2 PictureCorner(const Rect& area, const Mapping& m, int video_w, int video_h) {
    (void)video_w, (void)video_h;
    return ImVec2((float)std::max(area.x, m.ox), (float)std::max(area.y, m.oy));
}

void DrawVideoOverlay(ImDrawList* list, ImFont* font, const OverlayInput& in, std::vector<IntRect>* pills) {
    const float size = in.size;
    const float margin = size;
    const Rect& view = in.view;
    // A pill, noted for the logos when asked.
    auto pill = [&](ImVec2 position, const std::string& text, bool right_align) {
        const ImVec2 extent = DrawPill(list, font, size, position, text, right_align);
        if (pills) {
            const float x0 = right_align ? position.x - extent.x : position.x;
            const int ix = (int)std::floor(x0), iy = (int)std::floor(position.y);
            pills->push_back({ix, iy, (int)std::ceil(x0 + extent.x) - ix, (int)std::ceil(position.y + extent.y) - iy});
        }
    };
    if (in.layout == Layout::slider) {
        const float x = (float)in.divider_x;
        const float line = in.line;
        list->AddRectFilled(ImVec2(x - line, (float)view.y), ImVec2(x + line + 1.0f, (float)(view.y + view.h)),
                            IM_COL32(0, 0, 0, 120));
        list->AddRectFilled(ImVec2(x - std::floor(line / 2.0f), (float)view.y),
                            ImVec2(x + std::floor((line + 1.0f) / 2.0f), (float)(view.y + view.h)),
                            IM_COL32(255, 255, 255, 235));
        const ImVec2 corner = PictureCorner(in.areas.area[0], in.mapping[0], in.video_w, in.video_h);
        pill(ImVec2(corner.x + margin, corner.y + margin), in.label_a, false);
        const float right = (float)std::min(view.x + view.w, in.mapping[0].ox + in.video_w * in.mapping[0].scale);
        pill(ImVec2(right - margin, corner.y + margin), in.label_b, true);
    } else if (in.layout == Layout::side_by_side) {
        const ImVec2 a = PictureCorner(in.areas.area[0], in.mapping[0], in.video_w, in.video_h);
        const ImVec2 b = PictureCorner(in.areas.area[1], in.mapping[1], in.video_w, in.video_h);
        pill(ImVec2(a.x + margin, a.y + margin), in.label_a, false);
        pill(ImVec2(b.x + margin, b.y + margin), in.label_b, false);
    } else {
        const int side = in.layout == Layout::a_only ? 0 : 1;
        const ImVec2 c = PictureCorner(in.areas.area[side], in.mapping[side], in.video_w, in.video_h);
        pill(ImVec2(c.x + margin, c.y + margin), side == 0 ? in.label_a : in.label_b, false);
    }
    std::string text = in.caption;
    if (in.zoom > 1.05) {
        char zoom[32];
        std::snprintf(zoom, sizeof(zoom), "%.1fx", in.zoom);
        text += (text.empty() ? "" : "   ") + std::string(zoom);
    }
    if (!text.empty()) {
        const float pad = std::floor(size / 2.0f);
        const ImVec2 text_size = font->CalcTextSizeA(size, 1e9f, 0.0f, text.c_str());
        pill(ImVec2((float)(view.x + view.w / 2.0) - (text_size.x + 2.0f * pad) / 2.0f,
                    (float)(view.y + view.h) - margin - size - pad),
             text, false);
    }
    const char* missing = in.no_a ? "no recording of " : (in.no_b ? "no recording of " : nullptr);
    if (missing) {
        const std::string note = std::string(missing) + (in.no_a ? in.label_a : in.label_b) + " at this time";
        const float pad = std::floor(size / 2.0f);
        const ImVec2 text_size = font->CalcTextSizeA(size, 1e9f, 0.0f, note.c_str());
        pill(ImVec2((float)(view.x + view.w / 2.0) - (text_size.x + 2.0f * pad) / 2.0f,
                    (float)(view.y + view.h) - 3.0f * margin - 2.0f * size - 2.0f * pad),
             note, false);
    }
}

ImU32 AreaColor(size_t index) {
    static const ImU32 colors[] = {IM_COL32(255, 107, 107, 255), IM_COL32(255, 217, 61, 255), IM_COL32(107, 203, 119, 255),
                                   IM_COL32(77, 150, 255, 255),  IM_COL32(199, 125, 255, 255), IM_COL32(255, 159, 28, 255)};
    return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

void DrawNormalizedRect(ImDrawList* list, const Mapping& m, int video_w, int video_h, const double rect[4], ImU32 color,
                        bool selected) {
    double x0, y0, x1, y1;
    to_screen(m, video_w, video_h, rect[0], rect[1], x0, y0);
    to_screen(m, video_w, video_h, rect[0] + rect[2], rect[1] + rect[3], x1, y1);
    list->AddRect(ImVec2((float)x0, (float)y0), ImVec2((float)x1, (float)y1), color, 0.0f, 0, selected ? 3.0f : 2.0f);
}

void DrawAreaOutline(ImDrawList* list, ImFont* font, float size, const Mapping& m, int video_w, int video_h,
                     const Area& area, ImU32 color, bool selected, float alpha) {
    auto faded = [&](ImU32 c) {
        const float a = std::clamp(alpha, 0.0f, 1.0f) * (float)(c >> IM_COL32_A_SHIFT);
        return (c & ~IM_COL32_A_MASK) | ((ImU32)(a + 0.5f) << IM_COL32_A_SHIFT);
    };
    color = faded(color);
    DrawNormalizedRect(list, m, video_w, video_h, area.rect, color, selected);
    if (!area.label.empty()) {
        double x0, y0;
        to_screen(m, video_w, video_h, area.rect[0], area.rect[1], x0, y0);
        const ImVec2 at((float)x0 + 4.0f, (float)y0 - size - 4.0f);
        list->AddText(font, size, ImVec2(at.x + 1.0f, at.y + 1.0f), faded(IM_COL32(0, 0, 0, 200)), area.label.c_str());
        list->AddText(font, size, at, color, area.label.c_str());
    }
}

}  // namespace renderer::clips
