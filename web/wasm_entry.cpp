#include "pck/PckFile.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string s_manifest;
static std::string s_lastError;
static std::string s_lastPckVersion;

// Walk dir recursively, appending relative paths to out.
static void walkDir(const std::filesystem::path& dir, std::size_t rootLen, std::string& out)
{
    try {
        for(const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if(!entry.is_regular_file())
                continue;
            std::string full = entry.path().string();
            std::string rel  = full.substr(rootLen);
            if(!rel.empty() && rel.front() == '/')
                rel.erase(rel.begin());
            if(!out.empty())
                out += '\n';
            out += rel;
        }
    } catch(const std::filesystem::filesystem_error& e) {
        fprintf(stderr, "[godotpcktool] walkDir error: %s\n", e.what());
    }
}

// Write bytes to a MEMFS path. Sets s_lastError and returns false on failure.
static bool writeToMemfs(const uint8_t* data, std::size_t len, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    if(!f) {
        s_lastError = "Failed to open " + path + " for writing in virtual filesystem";
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if(!f.good()) {
        s_lastError = "Failed to write data to " + path + " in virtual filesystem";
        return false;
    }
    return true;
}

// Parses a "memfs_path\tpck_path\n..." manifest and adds each file to pck.
// Returns 0 on success, number of errors on failure (s_lastError set).
static int applyManifest(pcktool::PckFile& pck, const char* manifest)
{
    std::istringstream ss(manifest ? manifest : "");
    std::string line;
    int count = 0;
    while(std::getline(ss, line)) {
        if(line.empty())
            continue;
        const auto tab = line.find('\t');
        if(tab == std::string::npos) {
            s_lastError = "Malformed manifest line (missing tab): " + line;
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
        const std::string memfsPath = line.substr(0, tab);
        const std::string pckPath   = line.substr(tab + 1);
        fprintf(stderr, "[godotpcktool] staging %s -> %s\n", memfsPath.c_str(), pckPath.c_str());
        try {
            pck.AddSingleFile(memfsPath, pck.PreparePckPath(pckPath, ""), false);
        } catch(const std::exception& e) {
            s_lastError = std::string("Failed to add ") + pckPath + ": " + e.what();
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
        ++count;
    }
    return count > 0 ? 0 : -1; // -1 = empty manifest
}

extern "C" {

int extractPck(const uint8_t* data, std::size_t len)
{
    s_manifest.clear();
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] extractPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove_all("/out/", ec);
    std::filesystem::remove("/input.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    s_lastPckVersion = "Godot " + pck.GetGodotVersion() +
                       " (PCK format v" + std::to_string(pck.GetFormatVersion()) + ")";
    fprintf(stderr, "[godotpcktool] extractPck: %s\n", s_lastPckVersion.c_str());

    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return 3;
    }

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

const char* getLastPckVersion()
{
    return s_lastPckVersion.c_str();
}

// Returns newline-separated "path\tsize_bytes" manifest, or nullptr on error.
const char* listPck(const uint8_t* data, std::size_t len)
{
    s_manifest.clear();
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] listPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove_all("/out/", ec);
    std::filesystem::remove("/input.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return nullptr;
    }

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return nullptr;
    }
    s_lastPckVersion = "Godot " + pck.GetGodotVersion() +
                       " (PCK format v" + std::to_string(pck.GetFormatVersion()) + ")";
    fprintf(stderr, "[godotpcktool] listPck: %s\n", s_lastPckVersion.c_str());

    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return nullptr;
    }

    try {
        std::string root("/out/");
        for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if(!entry.is_regular_file())
                continue;
            std::string full = entry.path().string();
            std::string rel  = full.substr(root.size());
            if(!rel.empty() && rel.front() == '/')
                rel.erase(rel.begin());
            uint64_t sz = 0;
            try { sz = static_cast<uint64_t>(entry.file_size()); }
            catch(const std::filesystem::filesystem_error&) {}
            if(!s_manifest.empty())
                s_manifest += '\n';
            s_manifest += rel + '\t' + std::to_string(sz);
        }
    } catch(const std::filesystem::filesystem_error& e) {
        fprintf(stderr, "[godotpcktool] listPck walk error: %s\n", e.what());
    }

    const int count = s_manifest.empty() ?
        0 :
        static_cast<int>(std::count(s_manifest.begin(), s_manifest.end(), '\n')) + 1;
    fprintf(stderr, "[godotpcktool] listPck: %d files\n", count);

    if(s_manifest.empty()) {
        s_lastError = "No files found in this PCK.";
        return nullptr;
    }

    return s_manifest.c_str();
}

// Creates a new .pck from files pre-staged in WASM MEMFS.
// manifest: newline-separated "memfs_path\tpck_path" pairs.
// JS must write each file to MEMFS before calling this.
// Returns 0 on success, non-zero on error (call getLastError() for message).
int createPckFromManifest(const char* manifest,
                          uint32_t godot_major, uint32_t godot_minor, uint32_t godot_patch)
{
    s_lastError.clear();

    std::error_code ec;
    std::filesystem::remove("/output.pck", ec);

    pcktool::PckFile pck("/output.pck");
    pck.SetGodotVersion(godot_major, godot_minor, godot_patch);

    const int r = applyManifest(pck, manifest);
    if(r < 0) {
        s_lastError = "ZIP contains no files.";
        fprintf(stderr, "[godotpcktool] ERROR: empty manifest\n");
        return 1;
    }
    if(r > 0)
        return 1;

    if(!pck.Save()) {
        s_lastError = "Failed to write output PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 2;
    }

    fprintf(stderr, "[godotpcktool] createPckFromManifest: done\n");
    return 0;
}

} // extern "C"
