#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Pro build of ProsperoAI: three small, guarded source changes.

1. src/main.cpp reads vulkan_env.txt next to eboot.bin at start-up and sets each
   KEY=VALUE line as an environment variable, before any GPU work. That is how the
   ggml-vulkan tuning switches (and any llama.cpp / RADV switch) reach the app.
2. While a generation runs, the interface may sleep between frames
   (PROSPERO_UI_SLEEP_MS in vulkan_env.txt), so the 1080p interface drawing does
   not compete with inference for the GPU. 0 or unset = unchanged behaviour.
3. vulkan/gpt_runtime_router.cpp counts running generations for (2).

Usage: python3 prosperoai-ps5pro.py <ProsperoAI checkout>
Running it twice is harmless; it stops if the source no longer matches.
"""
from pathlib import Path
import sys

MARK = "ps5pro:"


def patch(path, edits):
    text = path.read_text()
    if MARK in text:
        print(f"{path}: already patched")
        return
    for before, after in edits:
        if text.count(before) != 1:
            raise SystemExit(f"{path}: expected exactly one match for:\n{before}")
        text = text.replace(before, after)
    path.write_text(text)
    print(f"{path}: patched")


def main():
    root = Path(sys.argv[1])

    patch(root / "vulkan/gpt_runtime_router.cpp", [
        ("#include <mutex>\n",
         "#include <mutex>\n#include <atomic>\n"),
        ("int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,\n"
         "                         const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,\n"
         "                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress)\n{\n",
         "// ps5pro: how many generations are running; the interface reads it (src/main.cpp).\n"
         "static std::atomic<int> ps5pro_generations{0};\n"
         "extern \"C\" int prospero_generation_active() { return ps5pro_generations.load(); }\n"
         "int gpt_runtime_generate(const gpt_runtime_message_t *messages, unsigned count,\n"
         "                         const gpt_runtime_settings_t &settings, char *output, std::size_t capacity,\n"
         "                         gpt_runtime_stats_t *stats, gpt_runtime_progress_fn progress)\n{\n"
         "    struct Ps5proActive {\n"
         "        Ps5proActive() { ps5pro_generations.fetch_add(1); }\n"
         "        ~Ps5proActive() { ps5pro_generations.fetch_sub(1); }\n"
         "    } ps5pro_active;\n"),
    ])

    patch(root / "src/main.cpp", [
        ("#include <vector>\n",
         """#include <vector>
#include <cstdlib>
#include <cstring>

// ps5pro: settings from vulkan_env.txt next to eboot.bin, set before any GPU work.
extern "C" int prospero_generation_active();
extern "C" int sceKernelUsleep(unsigned int);
static void ps5pro_load_env()
{
    std::FILE *file = std::fopen("/app0/vulkan_env.txt", "r");
    if (!file)
        return;
    char line[512];
    while (std::fgets(line, sizeof(line), file))
    {
        char *start = line;
        while (*start == ' ' || *start == '\\t')
            ++start;
        if (*start == '#' || *start == 0)
            continue;
        char *end = start + std::strlen(start);
        while (end > start && (end[-1] == '\\n' || end[-1] == '\\r' || end[-1] == ' '))
            *--end = 0;
        char *eq = std::strchr(start, '=');
        if (!eq || eq == start)
            continue;
        *eq = 0;
        setenv(start, eq + 1, 1);
        prospero::debug::line("app", "vulkan_env %s=%s", start, eq + 1);
    }
    std::fclose(file);
}
"""),
        ('    prospero::debug::start("/download0/ProsperoAI/logs");\n',
         '    prospero::debug::start("/download0/ProsperoAI/logs");\n'
         '    ps5pro_load_env();\n'),
        ("        if (!display.swap())\n            break;\n",
         """        if (!display.swap())
            break;
        {
            // ps5pro: draw fewer frames while a generation runs.
            static const unsigned ui_sleep_ms = []
            {
                const char *value = std::getenv("PROSPERO_UI_SLEEP_MS");
                return value ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0u;
            }();
            if (ui_sleep_ms && ui_sleep_ms <= 1000 && prospero_generation_active() > 0)
                sceKernelUsleep(ui_sleep_ms * 1000);
        }
"""),
    ])


if __name__ == "__main__":
    main()
