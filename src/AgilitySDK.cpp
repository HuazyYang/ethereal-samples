// Opts an executable into the D3D12 Agility SDK runtime (bin/D3D12/D3D12Core.dll).
//
// nvrhi marks acceleration-structure buffers with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
// which older system D3D12 runtimes reject ("Flags has unrecognized bits"). nvrhi's D3D12 graphics factory loads the
// Agility SDK itself (ID3D12SDKConfiguration1::CreateDeviceFactory, nvrhi ADR 0014); this sets the version it loads
// for every D3D12 device manager of the executable. Added to a target by ethereal_use_agility_sdk() in
// src/CMakeLists.txt.
#include <donut/app/DeviceManager.h>

#ifdef ETHEREAL_D3D12_SDK_VERSION
namespace
{
    struct RegisterAgilitySDK
    {
        RegisterAgilitySDK()
        {
            donut::app::SetDefaultD3D12AgilitySDK(ETHEREAL_D3D12_SDK_VERSION, ".\\D3D12\\");
        }
    } g_RegisterAgilitySDK;
}
#endif
