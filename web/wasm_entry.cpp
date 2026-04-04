#include "pck/PckFile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static std::string s_manifest;
static std::string s_lastError;

// Walk dir recursively, appending paths relative to rootLen to out.
static void walkDir(const std::filesystem::path& dir, std::size_t rootLen, std::string& out)
{
    for(const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if(!entry.is_regular_file())
            continue;
        std::string full = entry.path().string();
        // Strip leading root prefix to get relative path
        std::string rel = full.substr(rootLen);
        // Remove leading slash
        if(!rel.empty() && rel.front() == '/')
            rel.erase(rel.begin());
        if(!out.empty())
            out += '\n';
        out += rel;
    }
}

extern "C" {

int extractPck(const uint8_t* data, std::size_t len)
{
    s_manifest.clear();
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] extractPck: %zu bytes received\n", len);

    // Clean up any previous extraction
    std::error_code ec;
    std::filesystem::remove_all("/out/", ec);
    std::filesystem::remove("/input.pck", ec);

    // Write bytes to MEMFS
    {
        std::ofstream f("/input.pck", std::ios::binary);
        if(!f) {
            s_lastError = "Failed to write input file to virtual filesystem";
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        if(!f.good()) {
            s_lastError = "Failed to write input data to virtual filesystem";
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
    }
    fprintf(stderr, "[godotpcktool] Written to /input.pck\n");

    // Load PCK
    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] Loaded: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    // Extract all files to /out/
    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return 3;
    }
    fprintf(stderr, "[godotpcktool] Extract() complete\n");

    // Build manifest of relative paths
    std::string root("/out/");
    walkDir(root, root.size(), s_manifest);

    const int count = s_manifest.empty() ?
        0 :
        static_cast<int>(std::count(s_manifest.begin(), s_manifest.end(), '\n')) + 1;
    fprintf(stderr, "[godotpcktool] %d files extracted\n", count);

    if(s_manifest.empty()) {
        s_lastError = "No files found in this PCK.";
        return 4;
    }

    return 0;
}

const char* getExtractedFiles()
{
    return s_manifest.c_str();
}

const char* getLastError()
{
    return s_lastError.c_str();
}

} // extern "C"
