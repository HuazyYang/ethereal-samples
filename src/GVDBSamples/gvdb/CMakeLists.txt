# gvdb library + the gpdevice compute-abstraction layer it is built on.
# Headers sit next to their sources; the single include root is the parent
# directory (GVDBSamples), which serves every #include <gvdb/...>.
set(gvdb_root "${CMAKE_CURRENT_LIST_DIR}/..")

# --- gpdevice ---------------------------------------------------------------

add_library(gpdevice STATIC)
target_sources(
    gpdevice
    PRIVATE
        "GPDeviceCUDAImpl.cpp"
        "GPDeviceCUDAImpl.h"
        "GPDeviceNVRHI.cpp"
    PUBLIC
        "GPDevice.h"
        "GPDeviceCUDA.h"
        "GPDeviceNVRHI.h"
)
target_include_directories(gpdevice
  PUBLIC "$<BUILD_INTERFACE:${gvdb_root}>"
         "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(gpdevice
    PRIVATE CUDA::cuda_driver
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
set_target_properties(gpdevice PROPERTIES FOLDER "GVDBSamples")

# --- gvdb device code (PTX) -------------------------------------------------

add_library(gvdbPTX
    OBJECT
        "cuda_math.cuh"
        "GVDB.cuh"
        "GVDBDDA.cuh"
        "GVDBUpdateApron.cu"
)
set_target_properties(gvdbPTX PROPERTIES
     CUDA_PTX_COMPILATION ON
     CUDA_ARCHITECTURES "75"
     COMPILE_FLAGS  "-Xcompiler \"/wd 4819\""
)
target_compile_options(gvdbPTX PRIVATE --use_fast_math)
target_include_directories(gvdbPTX
    PUBLIC  "$<BUILD_INTERFACE:${gvdb_root}>"
            "$<INSTALL_INTERFACE:install>"
)
set_target_properties(gvdbPTX PROPERTIES FOLDER "GVDBSamples")

# --- gvdb host library ------------------------------------------------------

add_library(gvdb STATIC)
target_sources(
    gvdb
    PUBLIC
        "GVDB.h"
    PRIVATE
        "GVDBAllocator.h"
        "GVDBAllocator.cpp"
        "GVDB.cpp"
)
target_include_directories(gvdb
    PUBLIC "$<BUILD_INTERFACE:${gvdb_root}>"
           "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(gvdb PRIVATE "NOMINMAX=1")
# PUBLIC gpdevice: the public header <gvdb/GVDB.h> includes <gvdb/GPDevice.h>.
target_link_libraries(gvdb
    PRIVATE donut_engine
    PUBLIC donut_core gpdevice
)
add_dependencies(gvdb gvdbPTX)
set_target_properties(gvdb PROPERTIES FOLDER "GVDBSamples")
