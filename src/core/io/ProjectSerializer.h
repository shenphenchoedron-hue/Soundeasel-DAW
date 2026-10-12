#pragma once
// Versioned project format (JSON). Unknown fields are ignored and missing
// fields get defaults, so older/newer files load as gracefully as possible.
#include "model/Project.h"
#include <string>

namespace mc::io {

inline constexpr int kProjectFormatVersion = 2; // 2: audio tracks + Mains
inline constexpr const char* kProjectMagic = "midicomposer-project";

struct LoadResult {
    bool ok = false;
    std::string error;
    int fileVersion = 0;
    bool newerThanApp = false; // loaded best-effort
};

class ProjectSerializer {
public:
    // Audio file paths inside `baseDir` (the project's folder) are stored relative to it,
    // and relative paths are resolved against it when loading.
    static std::string toString(const model::Project&, const std::string& baseDir = {});
    static LoadResult fromString(const std::string&, model::Project& out, const std::string& baseDir = {});
    static bool saveFile(const model::Project&, const std::string& path, std::string* error = nullptr);
    static LoadResult loadFile(const std::string& path, model::Project& out);
};

} // namespace mc::io
