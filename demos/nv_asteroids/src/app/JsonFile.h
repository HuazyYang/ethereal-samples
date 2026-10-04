#pragma once

// Asteroids.exe: 0x140073C80, the framework's JSON file loader used by every demo data file.
// It takes an optional message callback; callers that probe for optional files (Camera%d.json) pass
// none and stay silent.
//
// deviation: donut::json::LoadFromFile always logs an error for missing files, so this small wrapper
// keeps the 2018 "silent" option. Errors are reported through donut::log when 'reportErrors' is set.

#include <filesystem>

namespace Json
{
    class Value;
}

namespace donut::vfs
{
    class IFileSystem;
}

namespace demo
{
    bool LoadJsonFile(donut::vfs::IFileSystem& fs, const std::filesystem::path& path, Json::Value& root,
        bool reportErrors);
}
