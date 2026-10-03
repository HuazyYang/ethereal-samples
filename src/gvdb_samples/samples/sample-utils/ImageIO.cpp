// PNG I/O through stb (the implementation macros are defined here, once).
#include "ImageIO.h"
#include "SampleTypes.h"
#include <donut/core/log.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996 4244 4267)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_NO_STDIO_WIDE
#include <stb_image.h>
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace SampleUtils {

using namespace donut;

bool savePNG(const std::filesystem::path &path, const uint8_t *rgba8, uint32_t width, uint32_t height) {
    std::string name = path.string();
    int ok = stbi_write_png(name.c_str(), int(width), int(height), 4, rgba8, int(width) * 4);
    if (!ok) log::error("savePNG: cannot write %s", name.c_str());
    return ok != 0;
}

bool loadPNG(const std::filesystem::path &path, std::vector<uint8_t> &rgba8, uint32_t &width, uint32_t &height) {
    std::string name = path.string();
    int w = 0, h = 0, comp = 0;
    stbi_uc *data = stbi_load(name.c_str(), &w, &h, &comp, 4);
    if (!data) {
        log::error("loadPNG: cannot read %s", name.c_str());
        return false;
    }
    width = uint32_t(w);
    height = uint32_t(h);
    rgba8.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

bool saveRenderBufferPNG(const std::filesystem::path &path, gp::IDevice *device, gp::IDeviceQueue *queue,
                         gp::IBuffer *rgba8, uint32_t width, uint32_t height) {
    if (!device || !queue || !rgba8) return false;
    const size_t bytes = size_t(width) * height * 4;
    gp::BufferDesc desc;
    desc.byteSize = bytes;
    desc.isStaging = true;
    nvrhi::AutoPtr<gp::IBuffer> staging;
    UT_V_GP(device->createBuffer(desc, &staging));
    UT_V_GP(queue->copyBufferRegion(staging, 0, rgba8, 0, bytes));
    syncQueue(device, queue);
    void *data = nullptr;
    UT_V_GP(device->mapBuffer(staging, &data));
    if (!data) return false;
    bool ok = savePNG(path, static_cast<const uint8_t *>(data), width, height);
    device->unmapBuffer(staging);
    return ok;
}

}  // namespace SampleUtils
