#include "pck/PckFile.h"
#include "FileFilter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

static std::string s_manifest;
static std::string s_lastError;

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
    fprintf(stderr, "[godotpcktool] Written to /input.pck\n");

    pcktool::PckFile pck("/input.pck");
    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] extractPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    if(!pck.Extract("/out/", false)) {
        s_lastError = "Extraction failed. The PCK may be encrypted or contain unsupported data.";
        fprintf(stderr, "[godotpcktool] ERROR: Extract() failed\n");
        return 3;
    }
    fprintf(stderr, "[godotpcktool] Extract() complete\n");

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

// Returns newline-separated "path\textracted_size_in_bytes" manifest, or nullptr on error.
// Sizes reflect the extracted (uncompressed) file sizes, not the packed sizes.
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
    fprintf(stderr, "[godotpcktool] listPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

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

// Loads PCK with optional regex filters, saves repacked result to /output.pck.
// include_regex / exclude_regex: empty string or null = no filter.
// Returns 0 on success, non-zero on error (call getLastError() for message).
int repackPck(const uint8_t* data, std::size_t len,
              const char* include_regex, const char* exclude_regex)
{
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] repackPck: %zu bytes received\n", len);

    std::error_code ec;
    std::filesystem::remove("/input.pck", ec);
    std::filesystem::remove("/output.pck", ec);

    if(!writeToMemfs(data, len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }

    pcktool::PckFile pck("/input.pck");

    const std::string inc = (include_regex && *include_regex) ? include_regex : "";
    const std::string exc = (exclude_regex && *exclude_regex) ? exclude_regex : "";

    if(!inc.empty() || !exc.empty()) {
        try {
            pcktool::FileFilter filter;
            if(!inc.empty())
                filter.SetIncludeRegexes({std::regex(inc)});
            if(!exc.empty())
                filter.SetExcludeRegexes({std::regex(exc)});
            pck.SetIncludeFilter([filter](const pcktool::PckFile::ContainedFile& f) {
                return filter.Include(f);
            });
        } catch(const std::regex_error& e) {
            s_lastError = std::string("Invalid regex: ") + e.what();
            fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
            return 1;
        }
    }

    if(!pck.Load()) {
        s_lastError =
            "Failed to load PCK. The file may be invalid, corrupted, or an unsupported version.";
        fprintf(stderr, "[godotpcktool] ERROR: Load() failed\n");
        return 2;
    }
    fprintf(stderr, "[godotpcktool] repackPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    pck.ChangePath("/output.pck");

    if(!pck.Save()) {
        s_lastError = "Failed to save repacked PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 3;
    }

    fprintf(stderr, "[godotpcktool] repackPck: done\n");
    return 0;
}

// Loads PCK, adds file_data at pck_path, saves modified PCK to /output.pck.
// pck_path: destination path inside the archive, e.g. "res://sprites/hero.png"
// Returns 0 on success, non-zero on error.
int addToPck(const uint8_t* pck_data, std::size_t pck_len,
             const uint8_t* file_data, std::size_t file_len,
             const char* pck_path)
{
    s_lastError.clear();

    fprintf(stderr, "[godotpcktool] addToPck: pck=%zu bytes, file=%zu bytes, path=%s\n",
        pck_len, file_len, pck_path ? pck_path : "(null)");

    std::error_code ec;
    std::filesystem::remove("/input.pck", ec);
    std::filesystem::remove("/add_input", ec);
    std::filesystem::remove("/output.pck", ec);

    if(!writeToMemfs(pck_data, pck_len, "/input.pck")) {
        fprintf(stderr, "[godotpcktool] ERROR: %s\n", s_lastError.c_str());
        return 1;
    }
    if(!writeToMemfs(file_data, file_len, "/add_input")) {
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
    fprintf(stderr, "[godotpcktool] addToPck: Godot %s, format version %u\n",
        pck.GetGodotVersion().c_str(), pck.GetFormatVersion());

    pck.ChangePath("/output.pck");
    pck.AddSingleFile("/add_input",
        pck.PreparePckPath(pck_path ? pck_path : "", ""), false);

    if(!pck.Save()) {
        s_lastError = "Failed to save modified PCK.";
        fprintf(stderr, "[godotpcktool] ERROR: Save() failed\n");
        return 3;
    }

    fprintf(stderr, "[godotpcktool] addToPck: done\n");
    return 0;
}

} // extern "C"
