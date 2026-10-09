// Build-time ROM extraction, for the ExtractAssets target and anything else that needs an
// archive without launching the game.
//
// soh links torch as a static library (USE_STANDALONE=OFF), which compiles out torch's own
// CLI, so this supplies the entry point. It calls the same SohTorch::Extract the game does.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "TorchExtract.h"

namespace fs = std::filesystem;

static void Usage(const char* argv0) {
    fprintf(stderr, "usage: %s --src <asset yml dir> --dest <output dir> --version <M.m.p> <rom|dir> [rom|dir...]\n",
            argv0);
}

static bool IsRom(const fs::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

// A directory argument extracts every rom directly inside it, which is how the target is
// normally driven: drop a vanilla and a master quest rom in, get oot.o2r and oot-mq.o2r.
static std::vector<std::string> CollectRoms(const std::vector<std::string>& args) {
    std::vector<std::string> roms;

    for (const auto& arg : args) {
        std::error_code ec;
        if (fs::is_directory(arg, ec)) {
            std::vector<std::string> found;
            for (fs::directory_iterator it(arg, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (it->is_regular_file(ec) && IsRom(it->path())) {
                    found.push_back(it->path().string());
                }
            }
            std::sort(found.begin(), found.end());
            roms.insert(roms.end(), found.begin(), found.end());
        } else {
            roms.push_back(arg);
        }
    }

    return roms;
}

int main(int argc, char** argv) {
    std::string src, dest, version;
    std::vector<std::string> romArgs;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing argument after %s\n", what);
                exit(1);
            }
            return argv[++i];
        };

        if (arg == "--src") {
            src = next("--src");
        } else if (arg == "--dest") {
            dest = next("--dest");
        } else if (arg == "--version") {
            version = next("--version");
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            Usage(argv[0]);
            return 1;
        } else {
            romArgs.push_back(arg);
        }
    }

    if (src.empty() || dest.empty() || version.empty() || romArgs.empty()) {
        Usage(argv[0]);
        return 1;
    }

    const std::vector<std::string> roms = CollectRoms(romArgs);
    if (roms.empty()) {
        fprintf(stderr, "no roms found in: ");
        for (const auto& arg : romArgs) {
            fprintf(stderr, "%s ", arg.c_str());
        }
        fprintf(stderr, "\n");
        return 1;
    }

    for (const auto& rom : roms) {
        std::ifstream input(rom, std::ios::binary);
        if (!input) {
            fprintf(stderr, "failed to open %s\n", rom.c_str());
            return 1;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad() || data.size() < 4 || data.size() % 4 != 0) {
            fprintf(stderr, "failed to read a valid ROM from %s\n", rom.c_str());
            return 1;
        }
        // Torch expects big-endian data, regardless of the dump's file extension.
        if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
            for (size_t i = 0; i < data.size(); i += 2) {
                std::swap(data[i], data[i + 1]);
            }
        } else if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
            for (size_t i = 0; i < data.size(); i += 4) {
                std::reverse(data.begin() + i, data.begin() + i + 4);
            }
        } else if (!(data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40)) {
            fprintf(stderr, "unrecognized ROM byte order in %s\n", rom.c_str());
            return 1;
        }
        // A fresh extraction per ROM; torch names the archive from config.yml.
        const std::string archive = SohTorch::Extract(std::move(data), src, dest, version, nullptr);
        if (archive.empty()) {
            fprintf(stderr, "failed to extract %s\n", rom.c_str());
            return 1;
        }
        printf("%s -> %s/%s\n", rom.c_str(), dest.c_str(), archive.c_str());
    }

    return 0;
}
