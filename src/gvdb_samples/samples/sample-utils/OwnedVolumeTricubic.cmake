# Production retained-snapshot tricubic renderer.
include("${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeRaycast.cmake")
if(NOT TARGET gvdb_owned_tricubic)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-tricubic.cfg" CONTENT
        "\"${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeTricubic.slang\" -T cs -E renderTricubic\n" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_owned_tricubic_shaders SLANG CUDA ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-tricubic.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_owned"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../gvdb/slang")
    add_library(gvdb_owned_tricubic STATIC "${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeTricubic.cpp")
    target_link_libraries(gvdb_owned_tricubic PUBLIC gvdb_owned_raycast)
    add_dependencies(gvdb_owned_tricubic gvdb_owned_tricubic_shaders)
    if(MSVC)
        target_compile_options(gvdb_owned_tricubic PRIVATE /utf-8)
    endif()
endif()

if(BUILD_TESTING AND NOT TARGET gvdb_owned_tricubic_tests)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-tricubic-probe.cfg" CONTENT
        "\"${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeTricubicProbe.slang\" -T cs -E probeTricubic\n" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_tricubic_probe_shaders SLANG CUDA ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-tricubic-probe.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_owned"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../gvdb/slang")
    add_executable(gvdb_owned_tricubic_tests "${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeTricubicTests.cpp")
    target_link_libraries(gvdb_owned_tricubic_tests PRIVATE gvdb_owned_tricubic donut_app)
    add_dependencies(gvdb_owned_tricubic_tests gvdb_tricubic_probe_shaders)
    ethereal_use_agility_sdk(gvdb_owned_tricubic_tests)
    if(MSVC)
        target_compile_options(gvdb_owned_tricubic_tests PRIVATE /utf-8)
    endif()
    if(NVRHI_WITH_CUDA)
        target_link_libraries(gvdb_owned_tricubic_tests PRIVATE nvrhi::cuda)
        target_compile_definitions(gvdb_owned_tricubic_tests PRIVATE ETHEREAL_TEST_CUDA=1)
    endif()
    foreach(api DX11 DX12 VULKAN CUDA)
        if(NVRHI_WITH_${api})
            if(api STREQUAL "VULKAN")
                set(_tricubic_argument -vk)
            elseif(api STREQUAL "DX11")
                set(_tricubic_argument -dx11)
            elseif(api STREQUAL "DX12")
                set(_tricubic_argument -dx12)
            else()
                set(_tricubic_argument -cuda)
            endif()
            add_test(NAME gvdb_owned_tricubic_${api} COMMAND gvdb_owned_tricubic_tests ${_tricubic_argument})
            set_tests_properties(gvdb_owned_tricubic_${api} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
        endif()
    endforeach()
    unset(_tricubic_argument)
endif()
