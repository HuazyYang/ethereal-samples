# SDK-free owned dense-to-sparse scalar resampling.
if(NOT TARGET gvdb_nvrhi_resample)
    include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiVolume.cmake")
    add_library(gvdb_nvrhi_resample STATIC "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResample.cpp")
    add_library(gvdb::nvrhi_resample ALIAS gvdb_nvrhi_resample)
    target_compile_features(gvdb_nvrhi_resample PUBLIC cxx_std_17)
    target_link_libraries(gvdb_nvrhi_resample PUBLIC gvdb::nvrhi_volume)
    target_include_directories(gvdb_nvrhi_resample PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
        "$<INSTALL_INTERFACE:include/gvdb>")
    set_target_properties(gvdb_nvrhi_resample PROPERTIES EXPORT_NAME nvrhi_resample POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(gvdb_nvrhi_resample PRIVATE /utf-8)
    endif()
    if(COMMAND ethereal_compile_shaders)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-resample.cfg"
            CONTENT "\"${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasResample.slang\" -T cs -E resampleOwnedAtlas\n" @ONLY)
        ethereal_compile_shaders(TARGET gvdb_nvrhi_resample_shaders SLANG CUDA ALL_PLATFORMS
            CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-resample.cfg"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_resample")
    endif()
endif()
