# Raw snapshot voxel/section helpers and production artifacts.
if(NOT TARGET gvdb_owned_view)
    include("${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeRaycast.cmake")
    set(_owned_view_config "")
    foreach(entry renderVoxel renderSection renderSection2D)
        string(APPEND _owned_view_config "\"${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeView.slang\" -T cs -E ${entry}\n")
    endforeach()
    foreach(entry raycastVolume raycastSurface)
        string(APPEND _owned_view_config "\"${CMAKE_CURRENT_LIST_DIR}/../../gvdb/slang/GVDBRawRaycast.slang\" -T cs -E ${entry}\n")
    endforeach()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-view.cfg" CONTENT "${_owned_view_config}" @ONLY)
    ethereal_compile_shaders(TARGET gvdb_owned_view_shaders SLANG CUDA ALL_PLATFORMS
        CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-view.cfg"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_slang"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../gvdb/slang" "${CMAKE_CURRENT_LIST_DIR}/../../../slang_compute")
    add_library(gvdb_owned_view STATIC "${CMAKE_CURRENT_LIST_DIR}/OwnedVolumeView.cpp")
    target_link_libraries(gvdb_owned_view PUBLIC gvdb_owned_raycast)
    target_include_directories(gvdb_owned_view PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
    add_dependencies(gvdb_owned_view gvdb_owned_view_shaders)
    if(MSVC)
        target_compile_options(gvdb_owned_view PRIVATE /utf-8)
    endif()
    unset(_owned_view_config)
endif()
