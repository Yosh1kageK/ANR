// What nr-replay's NGX generators (frame generation and SR/RR) and the SR test host share: a parameter map as a host
// keeps one, NVIDIA's NGX core, NGX's log, and DLL version strings. Windows.
#pragma once

#include <windows.h>

#include <d3d12.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

namespace replay {

using PFN_NgxInit = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                                   NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PFN_NgxParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PFN_NgxPopulate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using PFN_NgxCreate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                     NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NgxEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                       NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFN_NgxRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using PFN_NgxShutdown1 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

inline std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

inline std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string Hex(NVSDK_NGX_Result r) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", (unsigned)r);
    return text;
}

// A parameter map as a host keeps one for a provider's own exports: values by name, numbers readable as any number
// type, pointers as their own type or as void*.
class ParameterMap final : public NVSDK_NGX_Parameter {
public:
    void Set(const char* n, unsigned long long v) override { values_[n] = Number((double)v); }
    void Set(const char* n, float v) override { values_[n] = Number(v); }
    void Set(const char* n, double v) override { values_[n] = Number(v); }
    void Set(const char* n, unsigned int v) override { values_[n] = Number(v); }
    void Set(const char* n, int v) override { values_[n] = Number(v); }
    void Set(const char* n, ID3D11Resource* v) override { values_[n] = Pointer(v, D3D11); }
    void Set(const char* n, ID3D12Resource* v) override { values_[n] = Pointer(v, D3D12); }
    void Set(const char* n, void* v) override { values_[n] = Pointer(v, ANY); }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const override { return ReadPointer(n, v, D3D11); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const override { return ReadPointer(n, v, D3D12); }
    NVSDK_NGX_Result Get(const char* n, void** v) const override { return ReadPointer(n, v, ANY); }
    void Reset() override { values_.clear(); }

private:
    enum Kind { NUMBER, D3D11, D3D12, ANY };
    struct Value {
        Kind kind = NUMBER;
        double number = 0;  // every NGX number type converts exactly to and from double
        void* pointer = nullptr;
    };
    static Value Number(double v) { return {NUMBER, v, nullptr}; }
    static Value Pointer(void* p, Kind kind) { return {kind, 0, p}; }
    template <typename T>
    NVSDK_NGX_Result Read(const char* n, T* out) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind != NUMBER)
            return NVSDK_NGX_Result_Fail;
        *out = (T)it->second.number;
        return NVSDK_NGX_Result_Success;
    }
    template <typename T>
    NVSDK_NGX_Result ReadPointer(const char* n, T** out, Kind kind) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind == NUMBER ||
            (kind != ANY && it->second.kind != ANY && it->second.kind != kind))
            return NVSDK_NGX_Result_Fail;
        *out = (T*)it->second.pointer;
        return NVSDK_NGX_Result_Success;
    }
    std::unordered_map<std::string, Value> values_;
};

inline void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    std::fprintf(stderr, "NGX: %s\n", text.c_str());
}

// NVIDIA's NGX core: the folder the driver registers, else the system's search path (Wine prefixes: system32).
inline HMODULE LoadNgxCore(std::string& path) {
    wchar_t folder[MAX_PATH] = {};
    DWORD bytes = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                     RRF_RT_REG_SZ, nullptr, folder, &bytes) == ERROR_SUCCESS && folder[0]) {
        const std::wstring file = std::wstring(folder) + L"\\_nvngx.dll";
        if (HMODULE m = LoadLibraryW(file.c_str())) {
            path = Narrow(file);
            return m;
        }
    }
    HMODULE m = LoadLibraryW(L"_nvngx.dll");
    wchar_t file[MAX_PATH] = {};
    if (m && GetModuleFileNameW(m, file, MAX_PATH))
        path = Narrow(file);
    return m;
}

inline std::string ModulePath(HMODULE m) {
    wchar_t file[MAX_PATH] = {};
    return m && GetModuleFileNameW(m, file, MAX_PATH) ? Narrow(file) : std::string();
}

inline std::vector<uint8_t> VersionInfo(const std::string& path) {
    const std::wstring w = Wide(path);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
    std::vector<uint8_t> info(size);
    if (!size || !GetFileVersionInfoW(w.c_str(), 0, size, info.data()))
        info.clear();
    return info;
}

// A string of the DLL's version resource (FileDescription, ProductName ...), in its first language.
inline std::string VersionString(const std::vector<uint8_t>& info, const wchar_t* name) {
    struct Language {
        WORD language, codepage;
    }* languages = nullptr;
    UINT bytes = 0;
    if (info.empty() || !VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&languages, &bytes) ||
        bytes < sizeof(Language))
        return "";
    wchar_t key[128];
    std::swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\%ls", languages[0].language, languages[0].codepage, name);
    wchar_t* value = nullptr;
    return VerQueryValueW(info.data(), key, (void**)&value, &bytes) && value ? Narrow(value) : "";
}

}  // namespace replay
