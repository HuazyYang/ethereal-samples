# Offline portable GVDB shaders and their unified runtime correctness tests.
set(gvdb_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/slang")
file(READ "${gvdb_shader_dir}/kernels.json" kernel_manifest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gvdb_shader_dir}/kernels.json")
string(JSON kernel_count LENGTH "${kernel_manifest}" kernels)
math(EXPR last_kernel "${kernel_count} - 1")
set(config "")
foreach(index RANGE 0 ${last_kernel})
    string(JSON source GET "${kernel_manifest}" kernels ${index} source)
    string(JSON entry GET "${kernel_manifest}" kernels ${index} entry)
    string(APPEND config "\"${gvdb_shader_dir}/${source}\" -T cs -E ${entry}\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-shaders.cfg" CONTENT "${config}" @ONLY)
ethereal_compile_shaders(TARGET gvdb_slang_shaders SLANG CUDA ALL_PLATFORMS
    CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-shaders.cfg"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_slang"
    INCLUDE_DIRECTORIES "${gvdb_shader_dir}")
add_library(gvdb_slang_runtime STATIC ComputeRuntime.cpp ComputeRuntime.h)
target_sources(gvdb_slang_runtime PRIVATE ../gvdb_samples/gvdb/GVDBSlangPointKeys.cpp)
target_link_libraries(gvdb_slang_runtime PUBLIC nvrhi)
target_include_directories(gvdb_slang_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(gvdb_slang_tests ComputeTests.cpp)
target_link_libraries(gvdb_slang_tests PRIVATE gvdb_slang_runtime donut_app)
add_dependencies(gvdb_slang_tests gvdb_slang_shaders)
if(NVRHI_WITH_CUDA)
    target_link_libraries(gvdb_slang_tests PRIVATE nvrhi::cuda)
    target_compile_definitions(gvdb_slang_tests PRIVATE ETHEREAL_TEST_CUDA=1)
    add_test(NAME gvdb_slang_cuda COMMAND gvdb_slang_tests -cuda)
    set_tests_properties(gvdb_slang_cuda PROPERTIES TIMEOUT 180)
endif()
foreach(api DX11 DX12 VULKAN)
    if(NVRHI_WITH_${api})
        if(api STREQUAL "VULKAN")
            set(argument -vk)
        elseif(api STREQUAL "DX11")
            set(argument -dx11)
        else()
            set(argument -dx12)
        endif()
        add_test(NAME gvdb_slang_${api} COMMAND gvdb_slang_tests ${argument})
        set_tests_properties(gvdb_slang_${api} PROPERTIES TIMEOUT 180)
    endif()
endforeach()

if(MSVC)
    target_compile_definitions(gvdb_slang_tests PRIVATE _ENABLE_EXTENDED_ALIGNED_STORAGE)
endif()
target_sources(gvdb_slang_runtime PRIVATE ../gvdb_samples/gvdb/slang/GVDBPortableData.cpp ../gvdb_samples/gvdb/GVDBSlangVolume.cpp)

add_executable(gvdb_portable_layout_tests ../gvdb_samples/tests/PortableLayoutTests.cpp)
add_test(NAME gvdb_portable_layout COMMAND gvdb_portable_layout_tests)
add_executable(gvdb_portable_data_tests ../gvdb_samples/tests/PortableDataTests.cpp ../gvdb_samples/gvdb/slang/GVDBPortableData.cpp)
if(MSVC)
    target_compile_definitions(gvdb_portable_data_tests PRIVATE _ENABLE_EXTENDED_ALIGNED_STORAGE)
endif()
add_test(NAME gvdb_portable_data COMMAND gvdb_portable_data_tests)
target_sources(gvdb_slang_runtime PRIVATE ../gvdb_samples/gvdb/slang/GVDBPortableVbx.cpp ../gvdb_samples/gvdb/GVDBSlangSph.cpp)
add_executable(gvdb_portable_vbx_tests ../gvdb_samples/tests/PortableVbxTests.cpp)
target_link_libraries(gvdb_portable_vbx_tests PRIVATE gvdb_slang_runtime)
add_test(NAME gvdb_portable_vbx COMMAND gvdb_portable_vbx_tests "${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/samples/assets/explosion.vbx")
if(NOT TARGET render-to-file)
    add_subdirectory(../gvdb_samples/samples/render-to-file gvdb-render-to-file)
endif()
target_sources(gvdb_slang_runtime PRIVATE ../gvdb_samples/gvdb/GVDBSlangAtlas.cpp)

if(NOT TARGET sph_fluid_pool)
    add_subdirectory(../gvdb_samples/sph_fluid_pool gvdb-sph-fluid-pool)
endif()
target_sources(gvdb_slang_runtime PRIVATE ../gvdb_samples/gvdb/GVDBSlangPointFusion.cpp)

# Procedural ray tracing uses a library artifact; D3D11 keeps compute ray marching.
set(gvdb_rt_backends)
if(NVRHI_WITH_DX12)
    list(APPEND gvdb_rt_backends SLANG-DXIL)
endif()
if(NVRHI_WITH_VULKAN)
    list(APPEND gvdb_rt_backends SLANG-SPIRV)
endif()
if(NVRHI_WITH_OPTIX)
    list(APPEND gvdb_rt_backends SLANG-OPTIX)
endif()
target_sources(gvdb_slang_runtime PRIVATE
    ../gvdb_samples/gvdb/GVDBSlangRayTracing.cpp
    ../gvdb_samples/gvdb/GVDBSlangVoxelizer.cpp)
if(gvdb_rt_backends)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-rt.cfg"
        CONTENT "\"${gvdb_shader_dir}/GVDBRayTracing.slang\" -T lib\n" @ONLY)
    shadertool_add_shader_objects(TARGET gvdb_rt_shaders
        CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/gvdb-rt.cfg"
        BACKENDS ${gvdb_rt_backends} ARTIFACT
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_slang"
        INCLUDE_DIRECTORIES "${gvdb_shader_dir}"
        DEFINES_DXIL TARGET_D3D12 DEFINES_SPIRV SPIRV TARGET_VULKAN
        EXTRA_FLAGS_OPTIX "-I" "${NVRHI_OPTIX_INCLUDE_DIR}")
    add_executable(gvdb_rt_tests RTTests.cpp)
    target_link_libraries(gvdb_rt_tests PRIVATE gvdb_slang_runtime donut_app)
    ethereal_use_agility_sdk(gvdb_rt_tests)
    add_dependencies(gvdb_rt_tests gvdb_rt_shaders gvdb_slang_shaders)
    if(NVRHI_WITH_OPTIX)
        target_link_libraries(gvdb_rt_tests PRIVATE nvrhi::cuda)
        target_compile_definitions(gvdb_rt_tests PRIVATE ETHEREAL_TEST_CUDA=1)
        add_test(NAME gvdb_rt_CUDA COMMAND gvdb_rt_tests -cuda)
        set_tests_properties(gvdb_rt_CUDA PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()
    foreach(api DX12 VULKAN)
        if(NVRHI_WITH_${api})
            if(api STREQUAL "VULKAN")
                set(argument -vk)
            else()
                set(argument -dx12)
            endif()
            add_test(NAME gvdb_rt_${api} COMMAND gvdb_rt_tests ${argument})
            set_tests_properties(gvdb_rt_${api} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
add_executable(gvdb_raw_vbx_tests ../gvdb_samples/tests/RawVbxTests.cpp)
target_link_libraries(gvdb_raw_vbx_tests PRIVATE gvdb_slang_runtime)
add_test(NAME gvdb_raw_vbx COMMAND gvdb_raw_vbx_tests)

if(NOT TARGET render-kernel)
    add_subdirectory(../gvdb_samples/samples/render-kernel gvdb-render-kernel)
endif()

target_sources(gvdb_slang_runtime PRIVATE
    ../gvdb_samples/gvdb/GVDBSlangVelocity.cpp
    ../gvdb_samples/gvdb/GVDBSlangSubcells.cpp
    ../gvdb_samples/gvdb/GVDBSlangPointConversion.cpp
    ../gvdb_samples/gvdb/slang/GVDBTopologyBuilder.cpp)
target_sources(gvdb_slang_tests PRIVATE ExtendedComputeTests.cpp)
add_executable(gvdb_topology_builder_tests ../gvdb_samples/tests/TopologyBuilderTests.cpp)
target_link_libraries(gvdb_topology_builder_tests PRIVATE gvdb_slang_runtime)
add_test(NAME gvdb_topology_builder COMMAND gvdb_topology_builder_tests)

include(InteropTests.cmake)

include(VolumeViewer.cmake)

if(NOT TARGET resample)
    add_subdirectory(../gvdb_samples/samples/resample gvdb-resample)
endif()

if(NOT TARGET 3d-print)
    add_subdirectory(../gvdb_samples/samples/3d-print gvdb-3d-print)
endif()

if(NOT TARGET depth-map)
    add_subdirectory(../gvdb_samples/samples/depth-map gvdb-depth-map)
endif()

include(../gvdb_samples/samples/point-fusion/PointFusionTests.cmake)

if(NOT TARGET point-fusion)
    add_subdirectory(../gvdb_samples/samples/point-fusion gvdb-point-fusion)
endif()

include(../gvdb_samples/samples/point-cloud/PointCloudTests.cmake)

include(tests/resource-arrays/ResourceArrays.cmake)
include(../gvdb_samples/samples/sample-utils/SlangSceneRT.cmake)

if(NOT TARGET point-cloud)
    add_subdirectory(../gvdb_samples/samples/point-cloud gvdb-point-cloud)
endif()

if(TARGET gvdb_scene_rt AND NOT TARGET interactive-optix)
    add_subdirectory(../gvdb_samples/samples/interactive-optix gvdb-interactive-optix)
endif()

if(NOT TARGET fluid-surface)
    add_subdirectory(../gvdb_samples/samples/fluid-surface gvdb-fluid-surface)
endif()

if(NOT TARGET spray-deposit)
    add_subdirectory(../gvdb_samples/samples/spray-deposit gvdb-spray-deposit)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBHostVolume.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiVolume.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBHostImport.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiVoxelApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiVoxelFilterApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiAdoptionTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiAccumulationTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiSerializerTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBPointStreamConversion.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiPointApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiReductionApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiResampleApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiGatherApi.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiApronApi.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/samples/sample-utils/OwnedVolumeSurface.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiDownsampleApi.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiVoxelizeApi.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/../gvdb_samples/gvdb/GVDBNvrhiPointQuantizeApi.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../gvdb_samples/gvdb/GVDBNvrhiDepositApi.cmake")
