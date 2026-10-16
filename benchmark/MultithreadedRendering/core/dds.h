#pragma once
// Minimal DDS reader: header parsing and the subresource layout of 2D textures (BC1/BC2/BC3 and 8-bit
// RGBA/BGRA, optional mip chain). The pixel data stays in the file blob; the renderers upload it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <dxgiformat.h>

namespace MtScene
{

struct DdsSubresource
{
    uint32_t width = 0;      // in texels
    uint32_t height = 0;
    uint32_t rowPitch = 0;   // bytes per row (of blocks for BC formats)
    uint32_t rows = 0;       // rows (of blocks for BC formats)
    uint32_t sizeBytes = 0;  // rowPitch * rows
    const uint8_t* data = nullptr;
};

class DdsImage
{
public:
    bool Load(const std::filesystem::path& path, std::string& error);

    DXGI_FORMAT Format() const { return m_Format; }
    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }
    uint32_t MipLevels() const { return uint32_t(m_Mips.size()); }
    const DdsSubresource& Mip(uint32_t level) const { return m_Mips[level]; }
    bool IsBlockCompressed() const { return m_BlockBytes != 0; }

    // The sRGB variant of the format (for diffuse maps), the format itself when it has none.
    static DXGI_FORMAT MakeSRGB(DXGI_FORMAT format);

private:
    std::vector<uint8_t> m_Data;
    std::vector<DdsSubresource> m_Mips;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
    uint32_t m_Width = 0, m_Height = 0;
    uint32_t m_BlockBytes = 0;  // 0 = not block compressed
    uint32_t m_BytesPerPixel = 0;
};

} // namespace MtScene
