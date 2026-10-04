#include "app/JsonFile.h"

#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>

#include <nvrhi/core/autoptr.h>
#include <nvrhi/core/datablob.h>

#include <json/json.h>

#include <memory>
#include <string>

namespace demo
{
    bool LoadJsonFile(donut::vfs::IFileSystem& fs, const std::filesystem::path& path, Json::Value& root,
        bool reportErrors)
    {
        nvrhi::AutoPtr<nvrhi::IDataBlob> data;
        if (NVRHI_FAILED(fs.readFile(path, &data)) || !data)
        {
            if (reportErrors)
                donut::log::error("Couldn't read file %s", path.generic_string().c_str());
            return false;
        }

        Json::CharReaderBuilder builder;
        builder["collectComments"] = false;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

        const char* begin = static_cast<const char*>(data->GetDataPtr());
        std::string errors;
        if (!reader->parse(begin, begin + data->GetSize(), &root, &errors))
        {
            if (reportErrors)
                donut::log::error("Couldn't parse JSON file %s:\n%s", path.generic_string().c_str(), errors.c_str());
            return false;
        }

        return true;
    }
}
