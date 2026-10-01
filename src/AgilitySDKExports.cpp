// Opts an executable into the D3D12 Agility SDK runtime (bin/D3D12/D3D12Core.dll).
//
// nvrhi marks acceleration-structure buffers with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
// which older system D3D12 runtimes reject ("Flags has unrecognized bits"). The D3D12 loader looks for
// these two exports in the executable. Added to a target by ethereal_use_agility_sdk() in src/CMakeLists.txt.
#include <cstdint>

#ifdef ETHEREAL_D3D12_SDK_VERSION
extern "C" {
__declspec(dllexport) extern const uint32_t D3D12SDKVersion = ETHEREAL_D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif
