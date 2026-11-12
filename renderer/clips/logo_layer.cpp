#include "renderer/clips/logo_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <Detex/stb_image.h>  // NRIFramework's copy; its implementation is in the detex library NRIFramework links
#include <imgui_internal.h>   // RegisterUserTexture: how ImGui is told about a texture the application owns
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace {

std::vector<ImTextureData*>& Retiring() {
    static std::vector<ImTextureData*> textures;
    return textures;
}

// NRI keys its GPU copy of a texture by the ImTextureData's address and UniqueID: a new texture at an address a retired
// one had must not look like it.
int NextUniqueId() {
    static int id = 0;
    id = id % 1000 + 1;
    return id;
}

void Retire(ImTextureData* texture) {
    texture->WantDestroyNextFrame = true;
    Retiring().push_back(texture);
}

}  // namespace

bool load_logo_image(const std::string& path, LogoImage& image, std::string& error) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        error = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot read");
        return false;
    }
    image.w = w;
    image.h = h;
    image.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return true;
}

LogoLayer::~LogoLayer() {
    for (Entry& e : entries_)
        if (e.texture)
            Retire(e.texture);
}

void LogoLayer::CollectRetired() {
    std::vector<ImTextureData*>& list = Retiring();
    for (auto it = list.begin(); it != list.end();) {
        if ((*it)->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(*it);
            IM_DELETE(*it);
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

ImTextureData* LogoLayer::MakeTexture(const LogoImage& image) {
    ImTextureData* texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, image.w, image.h);
    std::memcpy(texture->Pixels, image.rgba.data(), (size_t)image.w * image.h * 4);
    texture->UseColors = true;
    texture->UsedRect.x = texture->UsedRect.y = 0;
    texture->UsedRect.w = (unsigned short)image.w;
    texture->UsedRect.h = (unsigned short)image.h;
    texture->UniqueID = NextUniqueId();
    ImGui::RegisterUserTexture(texture);
    return texture;
}

bool LogoLayer::empty() const {
    return count() == 0;
}

int LogoLayer::count() const {
    int n = 0;
    for (const Entry& e : entries_)
        n += e.texture != nullptr;
    return n;
}

void LogoLayer::Set(const std::vector<LogoItem>& items, const std::vector<std::string>& folders, int out_w, int out_h,
                    int settle) {
    frame_w_ = out_w;
    frame_h_ = out_h;
    std::string signature;
    for (const std::string& f : folders)
        signature += f + "\n";
    for (const LogoItem& i : items) {
        char text[256];
        std::snprintf(text, sizeof(text), "|%s|%.6f|%.6f|%.6f|%d", corner_name(i.corner), i.opacity, i.height, i.margin,
                      i.backdrop ? 1 : 0);
        signature += i.id + "|" + i.path + text + "\n";
    }
    const bool same_items = signature == signature_;
    const bool same_size = out_w == built_w_ && out_h == built_h_;
    if (same_items && same_size)
        return;
    if (same_items && settle > 0 && !entries_.empty()) {
        if (out_w != pending_w_ || out_h != pending_h_) {
            pending_w_ = out_w;
            pending_h_ = out_h;
            pending_calls_ = 0;
        }
        if (++pending_calls_ < settle)
            return;  // the old bitmaps, drawn scaled, until the size stays
    }
    signature_ = signature;
    built_w_ = out_w;
    built_h_ = out_h;
    pending_calls_ = 0;
    problems_.clear();
    // Textures of the same file at the same size stay.
    std::map<std::string, ImTextureData*> old;
    for (Entry& e : entries_)
        if (e.texture)
            old[e.key] = e.texture;
    std::vector<Entry> next;
    static std::set<std::string> logged;
    for (const LogoItem& item : items) {
        Entry e;
        e.item = item;
        const std::string path = find_logo_file(item.path, folders);
        if (path.empty()) {
            std::string where;
            for (const std::string& f : folders)
                where += (where.empty() ? "" : ", ") + f;
            const std::string message = "logo " + (item.id.empty() ? item.path : item.id) + ": " + item.path +
                                        " not found (looked in " + where + ")";
            problems_.push_back(message);
            if (logged.insert(message).second)
                spdlog::warn("{}", message);
            next.push_back(e);
            continue;
        }
        std::shared_ptr<LogoImage>& source = sources_[path];
        if (!source) {
            auto image = std::make_shared<LogoImage>();
            std::string error;
            if (!load_logo_image(path, *image, error)) {
                problems_.push_back("logo " + item.id + ": " + error);
                if (logged.insert(error).second)
                    spdlog::warn("logo {}: {}", item.id, error);
                sources_.erase(path);
                next.push_back(e);
                continue;
            }
            source = image;
        }
        e.aspect = (double)source->w / std::max(source->h, 1);
        const LogoPlace place = place_logo(item, out_w, out_h, e.aspect);
        e.w = place.logo.w;
        e.h = place.logo.h;
        e.key = path + "|" + std::to_string(e.w) + "x" + std::to_string(e.h);
        auto it = old.find(e.key);
        if (it != old.end()) {
            e.texture = it->second;
            old.erase(it);
        } else {
            e.texture = MakeTexture(resample_logo(*source, e.w, e.h));
        }
        next.push_back(e);
    }
    for (auto& [key, texture] : old)
        Retire(texture);
    entries_ = std::move(next);
}

void LogoLayer::Draw(ImDrawList* list, ImVec2 origin, const std::vector<IntRect>& pills) const {
    for (const Entry& e : entries_) {
        if (!e.texture)
            continue;
        const LogoPlace p = place_logo(e.item, frame_w_, frame_h_, e.aspect, pills);
        if (p.backdrop) {
            const float rounding = std::floor(0.25f * (float)p.box.h);
            list->AddRectFilled(ImVec2(origin.x + p.box.x, origin.y + p.box.y),
                                ImVec2(origin.x + p.box.x + p.box.w, origin.y + p.box.y + p.box.h), IM_COL32(0, 0, 0, 150),
                                rounding);
        }
        const int alpha = (int)std::lround(std::clamp(e.item.opacity, 0.0, 1.0) * 255.0);
        list->AddImage(e.texture->GetTexRef(), ImVec2(origin.x + p.logo.x, origin.y + p.logo.y),
                       ImVec2(origin.x + p.logo.x + p.logo.w, origin.y + p.logo.y + p.logo.h), ImVec2(0.0f, 0.0f),
                       ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, alpha));
    }
}

}  // namespace renderer::clips
