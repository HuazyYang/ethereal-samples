# Production snapshot renderer and artifacts. No BUILD_TESTING dependency.
if(NOT TARGET gvdb_owned_raycast)
    include("${CMAKE_CURRENT_LIST_DIR}/../../gvdb/GVDBNvrhiSerializer.cmake")
    set(_owned_shader_dir "${CMAKE_CURRENT_LIST_DIR}/../../gvdb/slang")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-raycast.cfg" CONTENT
        "\"${_owned_shader_dir}/GVDBAtlasRepack.slang\" -T cs -E relocateAtlas\n\"${_owned_shader_dir}/GVDBRawRaycast.slang\" -T cs -E raycastVolume\n\"${_owned_shader_dir}/GVDBRawRaycast.slang\" -T cs -E raycastSurface\n" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_owned_raycast_shaders SLANG CUDA ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-raycast.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_owned"
        INCLUDE_DIRECTORIES "${_owned_shader_dir}")
    add_library(gvdb_owned_raycast STATIC "${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeRaycast.cpp")
    target_compile_features(gvdb_owned_raycast PUBLIC cxx_std_17)
    target_link_libraries(gvdb_owned_raycast PUBLIC gvdb_slang_runtime gvdb::nvrhi_api)
    target_include_directories(gvdb_owned_raycast PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../..")
    add_dependencies(gvdb_owned_raycast gvdb_owned_raycast_shaders)
    if(MSVC)
        target_compile_options(gvdb_owned_raycast PRIVATE /utf-8)
    endif()
    unset(_owned_shader_dir)
endif()
