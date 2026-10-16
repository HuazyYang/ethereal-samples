#include "dds.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace MtScene
{

namespace
{

constexpr uint32_t kDdsMagic = 0x20534444; // "DDS "
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdsdMipmapCount = 0x20000;

constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat
{
    uint32_t size, flags, fourCC, rgbBitCount, rBitMask, gBitMask, bBitMask, aBitMask;
};
struct DdsHeader
{
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10
{
    uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
};
#pragma pack(pop)

} // namespace

DXGI_FORMAT DdsImage::MakeSRGB(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        default: return format;
    }
}

bool DdsImage::Load(const std::filesystem::path& path, std::string& error)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    {
        error = "cannot open " + path.string();
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    m_Data.resize(size > 0 ? size_t(size) : 0);
    const size_t read = m_Data.empty() ? 0 : fread(m_Data.data(), 1, m_Data.size(), f);
    fclose(f);
    if (m_Data.size() < 4 + sizeof(DdsHeader) || read != m_Data.size())
    {
        error = "not a DDS file: " + path.string();
        return false;
    }
    if (*reinterpret_cast<const uint32_t*>(m_Data.data()) != kDdsMagic)
    {
        error = "bad DDS magic: " + path.string();
        return false;
    }
    const DdsHeader& header = *reinterpret_cast<const DdsHeader*>(m_Data.data() + 4);
    if (header.size != sizeof(DdsHeader) || header.ddspf.size != sizeof(DdsPixelFormat))
    {
        error = "bad DDS header: " + path.string();
        return false;
    }

    size_t dataOffset = 4 + sizeof(DdsHeader);
    m_Format = DXGI_FORMAT_UNKNOWN;
    if ((header.ddspf.flags & kDdpfFourCC) && header.ddspf.fourCC == FourCC('D', 'X', '1', '0'))
    {
        if (m_Data.size() < dataOffset + sizeof(DdsHeaderDx10))
        {
            error = "truncated DX10 header: " + path.string();
            return false;
        }
        const DdsHeaderDx10& dx10 = *reinterpret_cast<const DdsHeaderDx10*>(m_Data.data() + dataOffset);
        dataOffset += sizeof(DdsHeaderDx10);
        if (dx10.resourceDimension != 3 /*TEXTURE2D*/ || dx10.arraySize > 1)
        {
            error = "only 2D non-array DDS textures are supported: " + path.string();
            return false;
        }
        m_Format = DXGI_FORMAT(dx10.dxgiFormat);
    }
    else if (header.ddspf.flags & kDdpfFourCC)
    {
        switch (header.ddspf.fourCC)
        {
            case FourCC('D', 'X', 'T', '1'): m_Format = DXGI_FORMAT_BC1_UNORM; break;
            case FourCC('D', 'X', 'T', '3'): m_Format = DXGI_FORMAT_BC2_UNORM; break;
            case FourCC('D', 'X', 'T', '5'): m_Format = DXGI_FORMAT_BC3_UNORM; break;
            case FourCC('A', 'T', 'I', '1'):
            case FourCC('B', 'C', '4', 'U'): m_Format = DXGI_FORMAT_BC4_UNORM; break;
            case FourCC('A', 'T', 'I', '2'):
            case FourCC('B', 'C', '5', 'U'): m_Format = DXGI_FORMAT_BC5_UNORM; break;
            default: break;
        }
    }
    else if ((header.ddspf.flags & kDdpfRgb) && header.ddspf.rgbBitCount == 32)
    {
        if (header.ddspf.rBitMask == 0x000000ff && header.ddspf.gBitMask == 0x0000ff00 && header.ddspf.bBitMask == 0x00ff0000)
            m_Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (header.ddspf.rBitMask == 0x00ff0000 && header.ddspf.gBitMask == 0x0000ff00 && header.ddspf.bBitMask == 0x000000ff)
            m_Format = header.ddspf.aBitMask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
    }

    switch (m_Format)
    {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
            m_BlockBytes = 8; m_BytesPerPixel = 0; break;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            m_BlockBytes = 16; m_BytesPerPixel = 0; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            m_BlockBytes = 0; m_BytesPerPixel = 4; break;
        default:
            error = "unsupported DDS pixel format in " + path.string();
            return false;
    }

    m_Width = header.width;
    m_Height = header.height;
    const uint32_t mipCount = (header.flags & kDdsdMipmapCount) ? std::max(1u, header.mipMapCount) : 1u;
    m_Mips.clear();
    uint32_t w = m_Width, h = m_Height;
    size_t offset = dataOffset;
    for (uint32_t level = 0; level < mipCount; ++level)
    {
        DdsSubresource mip;
        mip.width = w;
        mip.height = h;
        if (m_BlockBytes)
        {
            mip.rowPitch = std::max(1u, (w + 3) / 4) * m_BlockBytes;
            mip.rows = std::max(1u, (h + 3) / 4);
        }
        else
        {
            mip.rowPitch = w * m_BytesPerPixel;
            mip.rows = h;
        }
        mip.sizeBytes = mip.rowPitch * mip.rows;
        if (offset + mip.sizeBytes > m_Data.size())
        {
            error = "DDS pixel data truncated at mip " + std::to_string(level) + ": " + path.string();
            return false;
        }
        mip.data = m_Data.data() + offset;
        offset += mip.sizeBytes;
        m_Mips.push_back(mip);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return true;
}

} // namespace MtScene
