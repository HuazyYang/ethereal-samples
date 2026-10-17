/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "./asset_utils.h"
#include "graphics_bindings.h"

#include <filesystem>
#include <cstdlib>
#include <string>
#include <cmath>
#include <algorithm>
#include <vector>

std::filesystem::path FindDirectoryWithFile(const std::filesystem::path& startPath, const std::filesystem::path& relativeFilePath, int maxDepth)
{
	std::filesystem::path searchPath = "";

	for (int depth = 0; depth < maxDepth; depth++)
	{
		std::filesystem::path currentPath = startPath / searchPath / relativeFilePath;

		if (std::filesystem::is_directory(currentPath))
		{
			return currentPath.parent_path().lexically_normal();
		}

		searchPath = ".." / searchPath;
	}

	return "";
}

static std::filesystem::path g_mediaFolder = std::filesystem::path(WAVEWORKS_MEDIA_DIR);

std::filesystem::path const& GetMediaFolder()
{
	return g_mediaFolder;
}

std::string ReadProcessEnvironmentVariable(const char* name)
{
#ifdef _WIN32
	char* value = nullptr;
	size_t valueLength = 0;
	if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
		return {};
	std::string result(value);
	std::free(value);
	return result;
#else
	const char* value = std::getenv(name);
	return value ? value : "";
#endif
}

GenericImageType* LoadTexture(char const* Filename)
{
	GenericImageType *genericImage;
	DDSURFACEDESC2 ddsd;
	char filecode[4];
	FILE *fp;
	int factor, uncompressed_components, format_found;

	// try to open the file 
#ifdef WIN32
	fopen_s(&fp, Filename, "rb");
#else
	fp = fopen(Filename, "rb");
#endif
	if (fp == nullptr)
	{
		fprintf(stderr, "\nUnable to load texture file [%s]", Filename);
		return nullptr;
	}

	// verify the type of file 
	fread(filecode, 1, 4, fp);
	if (strncmp(filecode, "DDS ", 4) != 0)
	{
		fprintf(stderr, "\nThe file [%s] is not a dds file", Filename);
		fclose(fp);
		return nullptr;
	}

	// get the surface desc 
	fread(&ddsd, sizeof(ddsd), 1, fp);
	if ((ddsd.dwFlags & DDSD_PIXELFORMAT) != DDSD_PIXELFORMAT)
	{
		fprintf(stderr, "\nDDS header pixelformat field contains no valid data,\nunable to load texture (%s).", Filename);
		return nullptr;
	}
	if (((ddsd.dwFlags & DDSD_WIDTH) != DDSD_WIDTH) || ((ddsd.dwFlags & DDSD_HEIGHT) != DDSD_HEIGHT))
	{
		fprintf(stderr, "\nDDS header width/height fields contains no valid data,\nunable to load texture (%s).", Filename);
		return nullptr;
	}
	if ((ddsd.dwFlags & DDSD_MIPMAPCOUNT) != DDSD_MIPMAPCOUNT)
	{
		fprintf(stderr, "\nDDS header mipmapcount field contains no valid data,\nassuming 0 mipmaps (%s).", Filename);
		//return 0;
	}
	if (((ddsd.ddpfPixelFormat.dwFlags & DDPF_FOURCC) != DDPF_FOURCC) && ((ddsd.ddpfPixelFormat.dwFlags & DDPF_RGB) != DDPF_RGB))
	{
		fprintf(stderr, "\nDDS header pixelformat field contains no valid FOURCC and RGB data,\nunable to load texture (%s).", Filename);
		return nullptr;
	}
	if (((ddsd.ddpfPixelFormat.dwFlags & DDPF_FOURCC) == DDPF_FOURCC) && (ddsd.dwMipMapCount <= 1) && ((ddsd.ddpfPixelFormat.dwFourCC == FOURCC_DXT1) || (ddsd.ddpfPixelFormat.dwFourCC == FOURCC_DXT3) || (ddsd.ddpfPixelFormat.dwFourCC == FOURCC_DXT5)))
	{
		fprintf(stderr, "\nDDS header contains DXTx FOURCC code and no mipmaps,\nprogram does not support loading DXTx textures without mipmaps,\nunable to load texture (%s).", Filename);
		return nullptr;
	}
	genericImage = (GenericImageType*)malloc(sizeof(GenericImageType));
	memset(genericImage, 0, sizeof(GenericImageType));
	factor = 0;
	uncompressed_components = 0;
	format_found = 0;
	genericImage->width = ddsd.dwWidth;
	genericImage->height = ddsd.dwHeight;
	if ((ddsd.ddpfPixelFormat.dwFlags & DDPF_FOURCC) == DDPF_FOURCC) // if FOURCC code is valid
	{
		genericImage->format = ddsd.ddpfPixelFormat.dwFourCC;
		switch (ddsd.ddpfPixelFormat.dwFourCC)
		{
		case FOURCC_DXT1:
			if (ddsd.dwAlphaBitDepth != 0)
			{
				genericImage->components = 4;
			}
			else
			{
				genericImage->components = 3;
			}
			factor = 2;
			format_found = 1;
			break;
		case FOURCC_DXT3:
			genericImage->components = 4;
			factor = 4;
			format_found = 1;
			break;
		case FOURCC_DXT5:
			genericImage->components = 4;
			factor = 4;
			format_found = 1;
			break;
		default:
			break;
		}
	}

	if (format_found == 0) // Uncompressed format apparently
	{
		switch (ddsd.ddpfPixelFormat.dwRGBBitCount)
		{
		case 24:
			genericImage->format = 0;
			genericImage->components = 3;
			format_found = 2;
			break;
		case 32:
			genericImage->format = 0;
			genericImage->components = 4;
			format_found = 2;
		default:
			break;
		}
	}
	if (format_found == 1)  // found compressed format
	{
		genericImage->dataSize = ddsd.dwMipMapCount > 1 ? ddsd.dwLinearSize * factor  + (ddsd.dwLinearSize * factor / 2) : ddsd.dwLinearSize * factor;
		genericImage->baseRowPitch = ddsd.dwWidth * factor;
	}
	if (format_found == 2)
	{
		genericImage->dataSize = ddsd.dwMipMapCount > 1 ? ddsd.dwWidth*ddsd.dwHeight*genericImage->components + (ddsd.dwWidth*ddsd.dwHeight*genericImage->components) / 2 : ddsd.dwWidth*ddsd.dwHeight*genericImage->components;
		genericImage->baseRowPitch = ddsd.dwWidth*genericImage->components;
	}
	if (format_found == 0)
	{
		fprintf(stderr, "\nUnsupported DDS format,\nthe program only supports DXTx and RGB/RGBA formats containing 8 bits per component");
		free(genericImage);
		return nullptr;
	}

	genericImage->pixels = (unsigned char*)malloc(genericImage->dataSize);
	fread(genericImage->pixels, 1, genericImage->dataSize, fp);
	genericImage->numMipmaps = ddsd.dwMipMapCount > 0 ? ddsd.dwMipMapCount : 1;
	fclose(fp);

	return genericImage;
}

std::array<float, 3> IntegrateSkyIrradiance(const char* filename)
{
    std::array<float, 3> irradiance{};
    GenericImageType* sky = LoadTexture(filename);
    if (!sky) return irradiance;
    if (sky->format == 0 && sky->components == 4 && sky->width > 0 && sky->height > 0)
    {
        constexpr double pi = 3.141592653589793;
        for (int y = 0; y < sky->height; ++y)
        {
            const double theta = (y + 0.5) / sky->height * pi * 0.5;
            const float weight = float(std::cos(theta) * std::sin(theta) *
                (pi * 0.5 / sky->height) * (2.0 * pi / sky->width));
            for (int x = 0; x < sky->width; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const float encoded = sky->pixels[y * sky->baseRowPitch + x * 4 + 2 - c] / 255.f;
                    const float linear = encoded <= 0.04045f ? encoded / 12.92f :
                        std::pow((encoded + 0.055f) / 1.055f, 2.4f);
                    irradiance[c] += linear * weight;
                }
        }
    }
    free(sky->pixels);
    free(sky);
    return irradiance;
}

nvrhi::TextureHandle CreateTextureFromFile(nvrhi::IDevice* pDevice,
	nvrhi::ICommandList* pCommandList, char const* filename)
{
	return CreateTextureFromFile(
		pDevice, pCommandList, filename, nvrhi::Format::RGBA8_UNORM);
}

nvrhi::TextureHandle CreateTextureFromFile(nvrhi::IDevice* pDevice,
	nvrhi::ICommandList* pCommandList, char const* filename, nvrhi::Format format,
    bool generateMipmaps)
{
	GenericImageType* genericImage;
	nvrhi::TextureHandle result;

	genericImage = LoadTexture(filename);
	if (genericImage == nullptr) return nullptr;

	nvrhi::TextureDesc desc;
	desc.width = genericImage->width;
	desc.height = genericImage->height;
	desc.mipLevels = genericImage->numMipmaps;
	if (format != nvrhi::Format::RGBA8_UNORM &&
		format != nvrhi::Format::SRGBA8_UNORM)
	{
		free(genericImage->pixels);
		free(genericImage);
		return nullptr;
	}
	desc.format = format;
	desc.debugName = filename;

	if(genericImage->components != 4)
	{
		fprintf(stderr, "\nLoading textures with 3-component (RGB) formats is not supported");
		free(genericImage->pixels);
		free(genericImage);
		return nullptr;
	}
	if (genericImage->format != 0)
	{
		fprintf(stderr, "\nLoading textures with compressed formats is not supported");
		free(genericImage->pixels);
		free(genericImage);
		return nullptr;
	}

    // The beach scans ship with only a base level. Downsample albedo in linear
    // light; normal/data textures are averaged in their linear encoded domain.
    std::vector<std::vector<uint8_t>> generated;
    if (generateMipmaps && desc.mipLevels == 1)
    {
        uint32_t width = desc.width, height = desc.height;
        generated.emplace_back(genericImage->pixels,
            genericImage->pixels + size_t(width) * height * 4);
        while (width > 1 || height > 1)
        {
            uint32_t nextWidth = std::max(1u, width / 2);
            uint32_t nextHeight = std::max(1u, height / 2);
            std::vector<uint8_t> next(size_t(nextWidth) * nextHeight * 4);
            for (uint32_t y = 0; y < nextHeight; ++y)
                for (uint32_t x = 0; x < nextWidth; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        float value = 0.f;
                        for (uint32_t dy = 0; dy < 2; ++dy)
                            for (uint32_t dx = 0; dx < 2; ++dx)
                            {
                                float sample = generated.back()[
                                    (size_t(std::min(y * 2 + dy, height - 1)) * width +
                                    std::min(x * 2 + dx, width - 1)) * 4 + c] / 255.f;
                                if (format == nvrhi::Format::SRGBA8_UNORM && c < 3)
                                    sample = sample <= 0.04045f ? sample / 12.92f :
                                        std::pow((sample + 0.055f) / 1.055f, 2.4f);
                                value += sample * 0.25f;
                            }
                        if (format == nvrhi::Format::SRGBA8_UNORM && c < 3)
                            value = value <= 0.0031308f ? value * 12.92f :
                                1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
                        next[(size_t(y) * nextWidth + x) * 4 + c] =
                            uint8_t(std::clamp(std::round(value * 255.f), 0.f, 255.f));
                    }
            generated.push_back(std::move(next));
            width = nextWidth; height = nextHeight;
        }
        desc.mipLevels = uint32_t(generated.size());
    }
	pDevice->createTexture(desc, &result);
    if (!result)
    {
        free(genericImage->pixels); free(genericImage);
        return nullptr;
    }
	pCommandList->beginTrackingTextureState(result, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);

	uint8_t *data = genericImage->pixels;

	for (uint32_t i = 0; i < desc.mipLevels; i++)
	{
		pCommandList->writeTexture(result, 0, i,
            generated.empty() ? data : generated[i].data(), genericImage->baseRowPitch);
		
		if (generated.empty()) data += desc.height * genericImage->baseRowPitch;

		desc.width = std::max(1u, desc.width >> 1);
		desc.height = std::max(1u, desc.height >> 1);
		genericImage->baseRowPitch = desc.width * 4;
	} 

	free(genericImage->pixels);
	free(genericImage);
	pCommandList->setPermanentTextureState(result, nvrhi::ResourceStates::ShaderResource);
	pCommandList->commitBarriers();

	return result;
}
