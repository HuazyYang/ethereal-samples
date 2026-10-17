/*
* Copyright (c) 2008-2017, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef EXAMPLE_TEST_UTIL_H
#define EXAMPLE_TEST_UTIL_H

#include <stdio.h>
#include <assert.h>
#include "graphics_bindings.h"

#include <filesystem>
#include <string>
#include <array>

typedef struct
{
	int  width;
	int  height;
	int  components;
	uint32_t format;
	int  numMipmaps;
	uint8_t* pixels;
	uint32_t dataSize;
	uint32_t baseRowPitch;
} GenericImageType;

#ifndef _DDSURFACEDESC2
// following structs & defines are copied from ddraw.h

#ifndef DUMMYUNIONNAMEN
#if defined(__cplusplus) || !defined(NONAMELESSUNION)
#define DUMMYUNIONNAMEN(n)
#else
#define DUMMYUNIONNAMEN(n)      u##n
#endif
#endif

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
					((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |   \
					((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))
#endif //defined(MAKEFOURCC)

#define DDSD_PIXELFORMAT        0x00001000l
#define DDSD_CAPS               0x00000001l     
#define DDSD_HEIGHT             0x00000002l
#define DDSD_WIDTH              0x00000004l
#define DDSD_PITCH              0x00000008l
#define DDSD_MIPMAPCOUNT        0x00020000l
#define DDPF_FOURCC             0x00000004l
#define DDPF_RGB                0x00000040l
#define FOURCC_DXT1  (MAKEFOURCC('D','X','T','1'))
#define FOURCC_DXT2  (MAKEFOURCC('D','X','T','2'))
#define FOURCC_DXT3  (MAKEFOURCC('D','X','T','3'))
#define FOURCC_DXT4  (MAKEFOURCC('D','X','T','4'))
#define FOURCC_DXT5  (MAKEFOURCC('D','X','T','5'))

typedef struct _DDCOLORKEY
{
	uint32_t       dwColorSpaceLowValue;   // low boundary of color space that is to
										// be treated as Color Key, inclusive
	uint32_t       dwColorSpaceHighValue;  // high boundary of color space that is
										// to be treated as Color Key, inclusive
} DDCOLORKEY;

typedef struct _DDPIXELFORMAT
{
	uint32_t       dwSize;                 // size of structure
	uint32_t       dwFlags;                // pixel format flags
	uint32_t       dwFourCC;               // (FOURCC code)
	union
	{
		uint32_t   dwRGBBitCount;          // how many bits per pixel
		uint32_t   dwYUVBitCount;          // how many bits per pixel
		uint32_t   dwZBufferBitDepth;      // how many total bits/pixel in z buffer (including any stencil bits)
		uint32_t   dwAlphaBitDepth;        // how many bits for alpha channels
		uint32_t   dwLuminanceBitCount;    // how many bits per pixel
		uint32_t   dwBumpBitCount;         // how many bits per "buxel", total
		uint32_t   dwPrivateFormatBitCount;// Bits per pixel of private driver formats. Only valid in texture
										   // format list and if DDPF_D3DFORMAT is set
	} DUMMYUNIONNAMEN(1);
	union
	{
		uint32_t   dwRBitMask;             // mask for red bit
		uint32_t   dwYBitMask;             // mask for Y bits
		uint32_t   dwStencilBitDepth;      // how many stencil bits (note: dwZBufferBitDepth-dwStencilBitDepth is total Z-only bits)
		uint32_t   dwLuminanceBitMask;     // mask for luminance bits
		uint32_t   dwBumpDuBitMask;        // mask for bump map U delta bits
		uint32_t   dwOperations;           // DDPF_D3DFORMAT Operations
	} DUMMYUNIONNAMEN(2);
	union
	{
		uint32_t   dwGBitMask;             // mask for green bits
		uint32_t   dwUBitMask;             // mask for U bits
		uint32_t   dwZBitMask;             // mask for Z bits
		uint32_t   dwBumpDvBitMask;        // mask for bump map V delta bits
		struct
		{
			uint16_t    wFlipMSTypes;      // Multisample methods supported via flip for this D3DFORMAT
			uint16_t    wBltMSTypes;       // Multisample methods supported via blt for this D3DFORMAT
		} MultiSampleCaps;

	} DUMMYUNIONNAMEN(3);
	union
	{
		uint32_t   dwBBitMask;             // mask for blue bits
		uint32_t   dwVBitMask;             // mask for V bits
		uint32_t   dwStencilBitMask;       // mask for stencil bits
		uint32_t   dwBumpLuminanceBitMask; // mask for luminance in bump map
	} DUMMYUNIONNAMEN(4);
	union
	{
		uint32_t   dwRGBAlphaBitMask;      // mask for alpha channel
		uint32_t   dwYUVAlphaBitMask;      // mask for alpha channel
		uint32_t   dwLuminanceAlphaBitMask;// mask for alpha channel
		uint32_t   dwRGBZBitMask;          // mask for Z channel
		uint32_t   dwYUVZBitMask;          // mask for Z channel
	} DUMMYUNIONNAMEN(5);
} DDPIXELFORMAT;

typedef struct _DDSCAPS2
{
	uint32_t       dwCaps;         // capabilities of surface wanted
	uint32_t       dwCaps2;
	uint32_t       dwCaps3;
	union
	{
		uint32_t       dwCaps4;
		uint32_t       dwVolumeDepth;
	} DUMMYUNIONNAMEN(1);
} DDSCAPS2;

typedef struct _DDSURFACEDESC2
{
	uint32_t               dwSize;                 // size of the DDSURFACEDESC structure
	uint32_t               dwFlags;                // determines what fields are valid
	uint32_t               dwHeight;               // height of surface to be created
	uint32_t               dwWidth;                // width of input surface
	union
	{
		uint32_t           lPitch;                 // distance to start of next line (return value only)
		uint32_t           dwLinearSize;           // Formless late-allocated optimized surface size
	} DUMMYUNIONNAMEN(1);
	union
	{
		uint32_t           dwBackBufferCount;      // number of back buffers requested
		uint32_t           dwDepth;                // the depth if this is a volume texture 
	} DUMMYUNIONNAMEN(5);
	union
	{
		uint32_t           dwMipMapCount;          // number of mip-map levels requestde
												// dwZBufferBitDepth removed, use ddpfPixelFormat one instead
		uint32_t           dwRefreshRate;          // refresh rate (used when display mode is described)
		uint32_t           dwSrcVBHandle;          // The source used in VB::Optimize
	} DUMMYUNIONNAMEN(2);
	uint32_t               dwAlphaBitDepth;        // depth of alpha buffer requested
	uint32_t               dwReserved;             // reserved
	uint32_t               lpSurface;              // pointer to the associated surface memory
	union
	{
		DDCOLORKEY      ddckCKDestOverlay;      // color key for destination overlay use
		uint32_t           dwEmptyFaceColor;       // Physical color for empty cubemap faces
	} DUMMYUNIONNAMEN(3);
	DDCOLORKEY          ddckCKDestBlt;          // color key for destination blt use
	DDCOLORKEY          ddckCKSrcOverlay;       // color key for source overlay use
	DDCOLORKEY          ddckCKSrcBlt;           // color key for source blt use
	union
	{
		DDPIXELFORMAT   ddpfPixelFormat;        // pixel format description of the surface
		uint32_t           dwFVF;                  // vertex format description of vertex buffers
	} DUMMYUNIONNAMEN(4);
	DDSCAPS2            ddsCaps;                // direct draw surface capabilities
	uint32_t               dwTextureStage;         // stage in multitexture cascade
} DDSURFACEDESC2;
#endif //#ifndef _DDSURFACEDESC2

std::filesystem::path const& GetMediaFolder();
std::string ReadProcessEnvironmentVariable(const char* name);
// Cosine-weighted upper-hemisphere integral of the legacy BGRA sky panorama.
std::array<float, 3> IntegrateSkyIrradiance(const char* filename);

nvrhi::TextureHandle CreateTextureFromFile(nvrhi::IDevice* pDevice,
    nvrhi::ICommandList* pCommandList, char const* filename);
nvrhi::TextureHandle CreateTextureFromFile(nvrhi::IDevice* pDevice,
    nvrhi::ICommandList* pCommandList, char const* filename, nvrhi::Format format,
    bool generateMipmaps = false);


#endif /* EXAMPLE_TEST_UTIL_H */
