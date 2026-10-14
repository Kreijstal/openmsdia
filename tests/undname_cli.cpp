// undname_cli: OpenMSDIA's undecorator over a list of decorated names, in
// walker/name_probe.cpp's output format, for tools/undname_test.py:
//   undname_cli < names.txt > ours.json
// Builds and runs on any host (no Windows API).
#include <stdio.h>
#include <map>
#include <string>

#include "undname.h"

static const unsigned kFlags[] = {
    0x0,    0x1,    0x2,    0x4,     0x8,     0x10,    0x20,    0x40,    0x60,   0x80,   0x100,  0x200,  0x400,
    0x800,  0x1000, 0x2000, 0x4000,  0x8000,  0x10000, 0x20000, 0x40000, 0x80000, 0x87FE, 0x3000, 0x2002, 0xFFFF,
};

static std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

int main()
{
    std::map<std::string, std::string> out;
    char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        std::string n = line;
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        if (n.empty()) continue;
        std::string base = om::diaUndecorate(n, 0);
        std::string e = "{\"name\": " + jstr(n) + ", \"undecoratedName\": " +
                        jstr(om::diaUndecorate(n, om::UND_NO_PTR64)) + ", \"ex\": {\"0x0\": " + jstr(base);
        for (unsigned f : kFlags) {
            if (!f) continue;
            std::string u = om::diaUndecorate(n, f);
            if (u == base) continue;
            char k[16];
            snprintf(k, sizeof k, "0x%X", f);
            e += std::string(", ") + jstr(k) + ": " + jstr(u);
        }
        out[n] = e + "}}";
    }
    printf("{\n");
    size_t i = 0;
    for (auto &kv : out) printf("  %s: %s%s\n", jstr(kv.first).c_str(), kv.second.c_str(), ++i < out.size() ? "," : "");
    printf("}\n");
    return 0;
}
