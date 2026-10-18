# Production point conversion/gather/artifacts and optional independent fixtures.
include("${CMAKE_CURRENT_LIST_DIR}/../sample-utils/OwnedVolumeSurface.cmake")
# Ensure conversion fixtures exist before the public point API's guarded tests.
include("${CMAKE_CURRENT_LIST_DIR}/../../gvdb/GVDBPointStreamConversion.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../gvdb/GVDBNvrhiGatherApi.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../gvdb/GVDBNvrhiApronApi.cmake")
set(point_fusion_dir "${CMAKE_CURRENT_LIST_DIR}")
set(point_fusion_shader_config "")
foreach(entry scanBuildings compactScanHits)
    string(APPEND point_fusion_shader_config "\"${point_fusion_dir}/PointFusionScan.slang\" -T cs -E ${entry}\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/point-fusion.cfg" CONTENT "${point_fusion_shader_config}" @ONLY)
ethereal_compile_shaders(TARGET gvdb_point_fusion_shaders SLANG CUDA ALL_PLATFORMS
    CONFIG "${CMAKE_CURRENT_BINARY_DIR}/point-fusion.cfg"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_slang"
    INCLUDE_DIRECTORIES "${point_fusion_dir}/../../gvdb/slang")
add_library(gvdb_point_fusion STATIC "${point_fusion_dir}/PointFusionScanner.cpp" "${point_fusion_dir}/PointFusionVolume.cpp" "${point_fusion_dir}/PointFusionCity.cpp")
target_link_libraries(gvdb_point_fusion PUBLIC gvdb_slang_runtime gvdb_owned_surface gvdb::nvrhi_api PRIVATE donut_app)
target_include_directories(gvdb_point_fusion PUBLIC "${point_fusion_dir}" "${point_fusion_dir}/../..")
add_dependencies(gvdb_point_fusion gvdb_point_fusion_shaders gvdb_slang_shaders)
# Canonical conversion and gather are production dependencies, never test artifacts.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/point-fusion-owned.cfg" CONTENT
    "\"${point_fusion_dir}/../../gvdb/slang/GVDBPointStreamConversion.slang\" -T cs -E convertPointStream\n\"${point_fusion_dir}/../../gvdb/slang/GVDBAtlasGather.slang\" -T cs -E gatherOwnedAtlas\n" @ONLY)
ethereal_compile_shaders(TARGET gvdb_point_fusion_owned_shaders SLANG CUDA ALL_PLATFORMS
    CONFIG "${CMAKE_CURRENT_BINARY_DIR}/point-fusion-owned.cfg"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_owned"
    INCLUDE_DIRECTORIES "${point_fusion_dir}/../../gvdb/slang")
add_dependencies(gvdb_point_fusion gvdb_point_fusion_owned_shaders)
if(BUILD_TESTING)
    add_executable(gvdb_point_fusion_tests "${point_fusion_dir}/PointFusionTestMain.cpp" "${point_fusion_dir}/PointFusionTests.cpp")
    target_link_libraries(gvdb_point_fusion_tests PRIVATE gvdb_point_fusion donut_app)
    ethereal_use_agility_sdk(gvdb_point_fusion_tests)
    if(NVRHI_WITH_CUDA)
        target_link_libraries(gvdb_point_fusion_tests PRIVATE nvrhi::cuda)
        target_compile_definitions(gvdb_point_fusion_tests PRIVATE ETHEREAL_TEST_CUDA=1)
    endif()
    foreach(api DX11 DX12 VULKAN CUDA)
        if(NVRHI_WITH_${api})
            if(api STREQUAL "VULKAN")
                set(argument -vk)
            elseif(api STREQUAL "DX11")
                set(argument -dx11)
            elseif(api STREQUAL "DX12")
                set(argument -dx12)
            else()
                set(argument -cuda)
            endif()
            add_test(NAME gvdb_point_fusion_${api} COMMAND gvdb_point_fusion_tests ${argument})
            set_tests_properties(gvdb_point_fusion_${api} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()

endif()
