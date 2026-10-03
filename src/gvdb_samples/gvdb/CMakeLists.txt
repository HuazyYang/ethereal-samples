# gvdb library: the gp compute abstraction (gpdevice) and the GVDB data
# structure with its operation objects and scene-graph integration (gvdb).
# Headers sit next to their sources and are included as <gvdb/...>.

# --- gpdevice ---------------------------------------------------------------
# CUDA backend, nvrhi interop, Thrust helpers and (optional) OptiX backend.

add_library(gpdevice STATIC
    GPDevice.h
    GPDeviceCUDA.h
    GPDeviceCUDAImpl.h
    GPDeviceCUDAImpl.cpp
    GPDeviceCUDAUtils.h
    GPDeviceCUDAUtils.cpp
    GPDeviceCUDAUtils.cu
    GPDeviceNVRHI.h
    GPDeviceNVRHI.cpp
    GPDeviceOptiX.h
    GPDeviceOptiXImpl.h
    GPDeviceOptiXImpl.cpp
)
target_include_directories(gpdevice
    PUBLIC "$<BUILD_INTERFACE:${GVDB_INCLUDE_ROOT}>"
           "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(gpdevice
    PRIVATE CUDA::cuda_driver CUDA::cudart_static
    PUBLIC donut_core nvrhi
)
# nvrhi vendors Vulkan-Headers / DirectX-Headers; the exported names depend on
# whether they come from nvrhi's thirdparty copies or from an external package.
if (TARGET Vulkan-Headers)
    target_link_libraries(gpdevice PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(gpdevice PRIVATE Vulkan::Headers)
endif()
if (TARGET Microsoft::DirectX-Headers)
    target_link_libraries(gpdevice PRIVATE Microsoft::DirectX-Headers)
elseif (TARGET DirectX-Headers)
    target_link_libraries(gpdevice PRIVATE DirectX-Headers)
endif()
target_compile_definitions(gpdevice PRIVATE "VK_USE_PLATFORM_WIN32_KHR=1")
if (ETHEREAL_OPTIX_FOUND)
    target_include_directories(gpdevice PUBLIC "${OptiX_INCLUDE}")
    target_compile_definitions(gpdevice PUBLIC "GVDB_WITH_OPTIX=1")
endif()
set_target_properties(gpdevice PROPERTIES FOLDER "GVDBSamples" CUDA_ARCHITECTURES "${GVDB_PTX_ARCH}")

# --- gvdb device code (PTX) -------------------------------------------------

gvdb_add_ptx(gvdb_ptx
    SOURCES
        "kernels/GVDBOperators.cu"
        "kernels/GVDBParticles.cu"
)
target_sources(gvdb_ptx_objects PRIVATE "GVDB.cuh" "GVDBDDA.cuh" "cuda_math.cuh")

# --- gvdb host library ------------------------------------------------------

add_library(gvdb STATIC
    GVDB.h
    GVDBTypes.cpp
    GVDBAllocator.h
    GVDBAllocator.cpp
    GVDBVolume.cpp
    GVDBVoxelOps.cpp
    GVDBPointOps.cpp
    GVDBVoxelizer.cpp
    GVDBSerializer.cpp
    GVDBScene.h
    GVDBScene.cpp
)
target_include_directories(gvdb
    PUBLIC "$<BUILD_INTERFACE:${GVDB_INCLUDE_ROOT}>"
           "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(gvdb PRIVATE "NOMINMAX=1")
# PUBLIC gpdevice: <gvdb/GVDB.h> includes <gvdb/GPDevice.h>; PUBLIC donut_engine:
# <gvdb/GVDBScene.h> derives from the Donut scene graph.
target_link_libraries(gvdb
    PUBLIC donut_core donut_engine gpdevice
)
add_dependencies(gvdb gvdb_ptx)
set_target_properties(gvdb PROPERTIES FOLDER "GVDBSamples")
