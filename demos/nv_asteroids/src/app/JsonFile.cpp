#include "app/JsonFile.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>

#include <json/json.h>

#include <memory>
#include <string>

namespace demo
{
    bool LoadJsonFile(donut::vfs::IFileSystem& fs, const std::filesystem::path& path, Json::Value& root,
        bool reportErrors)
    {
        std::shared_ptr<donut::vfs::IBlob> data = fs.readFile(path);
        if (!data)
        {
            if (reportErrors)
                donut::log::error("Couldn't read file %s", path.generic_string().c_str());
            return false;
        }

        Json::CharReaderBuilder builder;
        builder["collectComments"] = false;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

        const char* begin = static_cast<const char*>(data->data());
        std::string errors;
        if (!reader->parse(begin, begin + data->size(), &root, &errors))
        {
            if (reportErrors)
                donut::log::error("Couldn't parse JSON file %s:\n%s", path.generic_string().c_str(), errors.c_str());
            return false;
        }

        return true;
    }
}
