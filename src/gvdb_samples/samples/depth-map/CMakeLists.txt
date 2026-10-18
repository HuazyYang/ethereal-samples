# Original CUDA sample remains available when the Slang migration is disabled.
if(ETHEREAL_WITH_SLANG AND TARGET gvdb_slang_runtime)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../sample-utils/OwnedVolumeRaycast.cmake")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth-raster.cfg" CONTENT
        "\"${CMAKE_CURRENT_SOURCE_DIR}/DepthRaster.slang\" -T vs -E meshVS\n\"${CMAKE_CURRENT_SOURCE_DIR}/DepthRaster.slang\" -T ps -E meshPS\n\"${CMAKE_CURRENT_SOURCE_DIR}/DepthRaster.slang\" -T vs -E compositeVS\n\"${CMAKE_CURRENT_SOURCE_DIR}/DepthRaster.slang\" -T ps -E compositePS\n" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_depth_raster_shaders SLANG ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/depth-raster.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/depth_map")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth-clip.cfg" CONTENT
        "\"${CMAKE_CURRENT_SOURCE_DIR}/DepthClip.slang\" -T cs -E clipVolume\n" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_depth_clip_shaders SLANG CUDA ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/depth-clip.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/depth_map"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../gvdb/slang")
    add_executable(depth-map main.cpp DepthMap.cpp DepthMapTests.cpp ../sample-utils/ObjLoader.cpp)
    target_link_libraries(depth-map PRIVATE gvdb_owned_raycast donut_app nvrhi::interop stb)
    target_include_directories(depth-map PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_definitions(depth-map PRIVATE "GVDB_SAMPLES_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../assets\"")
    add_dependencies(depth-map gvdb_depth_raster_shaders gvdb_depth_clip_shaders)
    ethereal_use_agility_sdk(depth-map)
    if(NVRHI_WITH_CUDA)
        target_link_libraries(depth-map PRIVATE nvrhi::cuda)
        target_compile_definitions(depth-map PRIVATE ETHEREAL_TEST_CUDA=1)
    else()
        target_compile_definitions(depth-map PRIVATE ETHEREAL_TEST_CUDA=0)
    endif()
    if(BUILD_TESTING)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth-typed.cfg" CONTENT
            "\"${CMAKE_CURRENT_SOURCE_DIR}/DepthClipTyped.slang\" -T cs -E clipVolume\n" @ONLY)
        ethereal_compile_shaders(TARGET gvdb_depth_reference_shaders SLANG CUDA ALL_PLATFORMS
            CONFIG "${CMAKE_CURRENT_BINARY_DIR}/depth-typed.cfg"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/depth_map"
            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../gvdb/slang")
        add_executable(gvdb_depth_owned_tests main.cpp DepthMap.cpp DepthMapTests.cpp DepthMapReference.cpp ../sample-utils/ObjLoader.cpp)
        target_link_libraries(gvdb_depth_owned_tests PRIVATE gvdb_owned_raycast donut_app nvrhi::interop stb)
        target_include_directories(gvdb_depth_owned_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
        target_compile_definitions(gvdb_depth_owned_tests PRIVATE ETHEREAL_DEPTH_REFERENCE_TESTS=1
            "GVDB_SAMPLES_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../assets\"")
        add_dependencies(gvdb_depth_owned_tests gvdb_depth_raster_shaders gvdb_depth_clip_shaders
            gvdb_depth_reference_shaders gvdb_slang_shaders)
        ethereal_use_agility_sdk(gvdb_depth_owned_tests)
        if(NVRHI_WITH_CUDA)
            target_link_libraries(gvdb_depth_owned_tests PRIVATE nvrhi::cuda)
            target_compile_definitions(gvdb_depth_owned_tests PRIVATE ETHEREAL_TEST_CUDA=1)
        else()
            target_compile_definitions(gvdb_depth_owned_tests PRIVATE ETHEREAL_TEST_CUDA=0)
        endif()
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
            set(compute_modes graphics)
            if(NVRHI_WITH_CUDA)
                list(APPEND compute_modes cuda)
            endif()
            foreach(compute IN LISTS compute_modes)
                add_test(NAME gvdb_depth_map_analytic_${api}_${compute} COMMAND depth-map ${argument} --compute-api ${compute} --self-test)
                add_test(NAME gvdb_depth_map_asset_${api}_${compute} COMMAND depth-map ${argument} --compute-api ${compute}
                    --headless --width 160 --height 120 --output "${CMAKE_CURRENT_BINARY_DIR}/depth-${api}-${compute}.png")
                if(BUILD_TESTING)
                    add_test(NAME gvdb_depth_map_owned_analytic_${api}_${compute} COMMAND gvdb_depth_owned_tests ${argument} --compute-api ${compute} --self-test)
                    add_test(NAME gvdb_depth_map_owned_asset_${api}_${compute} COMMAND gvdb_depth_owned_tests ${argument} --compute-api ${compute}
                        --headless --width 160 --height 120 --output "${CMAKE_CURRENT_BINARY_DIR}/depth-owned-${api}-${compute}.png")
                    set_tests_properties(gvdb_depth_map_owned_analytic_${api}_${compute} gvdb_depth_map_owned_asset_${api}_${compute} PROPERTIES TIMEOUT 180)
                endif()
                set_tests_properties(gvdb_depth_map_analytic_${api}_${compute} gvdb_depth_map_asset_${api}_${compute} PROPERTIES TIMEOUT 180)
            endforeach()
        endif()
    endforeach()
else()
    gvdb_add_sample(depth-map SOURCES legacy-main.cpp)
endif()
