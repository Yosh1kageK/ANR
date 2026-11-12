// embed <input> <array name> <output header>: the input's bytes as "unsigned char <name>[] = {...};".
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: embed <input> <array name> <output header>\n");
        return 2;
    }
    std::FILE* in = std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[3], "w");
    if (!in || !out) {
        std::fprintf(stderr, "embed: cannot open %s or %s\n", argv[1], argv[3]);
        return 1;
    }
    std::fprintf(out, "unsigned char %s[] = {", argv[2]);
    int c;
    for (unsigned long n = 0; (c = std::fgetc(in)) != EOF; n++)
        std::fprintf(out, "%s%d,", n % 32 ? "" : "\n", c);
    std::fprintf(out, "\n};\n");
    std::fclose(in);
    return std::fclose(out) == 0 ? 0 : 1;
}
