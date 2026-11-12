// Removes the debug instructions of a SPIR-V module: OpSource, OpSourceContinued, OpSourceExtension, OpName,
// OpMemberName, OpLine, OpNoLine and OpModuleProcessed, OpString where nothing else uses it, and the instructions of
// NonSemantic debug-info sets (with their imports). The rest stays word for word, so the module runs as before; the
// host binds by the reflection JSON, not by names.
//
//   spirv_strip IN.spv OUT.spv

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint32_t MAGIC = 0x07230203;
enum : uint32_t {
    OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7,
    OpLine = 8, OpExtInstImport = 11, OpExtInst = 12, OpNoLine = 317, OpModuleProcessed = 330,
};

bool DebugSet(const uint32_t* words, uint32_t count) {
    const char* name = reinterpret_cast<const char*>(words + 2);
    const size_t bytes = (count - 2) * 4;
    const std::string text(name, strnlen(name, bytes));
    return text.rfind("NonSemantic.Shader.DebugInfo", 0) == 0 || text.rfind("NonSemantic.DebugInfo", 0) == 0;
}

int Fail(const char* what) {
    std::fprintf(stderr, "spirv_strip: %s\n", what);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return Fail("usage: spirv_strip IN.spv OUT.spv");
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 20 || bytes.size() % 4)
        return Fail("not a SPIR-V module");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] != MAGIC)
        return Fail("not a little-endian SPIR-V module");

    // First pass: the debug sets' IDs and the IDs that instructions other than debug ones use (for OpString).
    std::set<uint32_t> debug_sets, strings, used;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        if (count == 0 || i + count > words.size())
            return Fail("malformed instruction");
        if (op == OpExtInstImport && DebugSet(&words[i], count))
            debug_sets.insert(words[i + 1]);
        else if (op == OpString)
            strings.insert(words[i + 1]);
        i += count;
    }
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        const bool debug = op == OpSource || op == OpSourceContinued || op == OpSourceExtension || op == OpName ||
                           op == OpMemberName || op == OpLine || op == OpNoLine || op == OpModuleProcessed ||
                           op == OpString || (op == OpExtInstImport && debug_sets.count(words[i + 1])) ||
                           (op == OpExtInst && count > 3 && debug_sets.count(words[i + 3]));
        if (!debug)
            for (uint32_t k = 1; k < count; k++)
                if (strings.count(words[i + k]))
                    used.insert(words[i + k]);  // an operand that may be a string's ID (kept to be safe)
        i += count;
    }
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
        bool drop = op == OpSource || op == OpSourceContinued || op == OpSourceExtension || op == OpName ||
                    op == OpMemberName || op == OpLine || op == OpNoLine || op == OpModuleProcessed ||
                    (op == OpString && !used.count(words[i + 1])) ||
                    (op == OpExtInstImport && debug_sets.count(words[i + 1])) ||
                    (op == OpExtInst && count > 3 && debug_sets.count(words[i + 3]));
        if (!drop)
            out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        i += count;
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), out.size() * 4);
    return o ? 0 : Fail("cannot write the output");
}
