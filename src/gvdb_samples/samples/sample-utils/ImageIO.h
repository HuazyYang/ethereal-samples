#ifndef SAMPLE_UTILS_IMAGEIO_H
#define SAMPLE_UTILS_IMAGEIO_H
// PNG read / write (stb_image, stb_image_write from Donut's thirdparty).
#include <gvdb/GPDevice.h>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace SampleUtils {

bool savePNG(const std::filesystem::path &path, const uint8_t *rgba8, uint32_t width, uint32_t height);
bool loadPNG(const std::filesystem::path &path, std::vector<uint8_t> &rgba8, uint32_t &width, uint32_t &height);

// Reads a device RGBA8 render buffer (width*height*4) back and saves it.
bool saveRenderBufferPNG(const std::filesystem::path &path, donut::gp::IDevice *device, donut::gp::IDeviceQueue *queue,
                         donut::gp::IBuffer *rgba8, uint32_t width, uint32_t height);

}  // namespace SampleUtils

#endif /* SAMPLE_UTILS_IMAGEIO_H */
