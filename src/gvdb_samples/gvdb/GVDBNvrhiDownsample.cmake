# Dense device operation: no GVDB topology or volume instance is required.
if(NOT TARGET gvdb_nvrhi_downsample)
    add_library(gvdb_nvrhi_downsample STATIC "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiDownsample.cpp")
    add_library(gvdb::nvrhi_downsample ALIAS gvdb_nvrhi_downsample)
    target_compile_features(gvdb_nvrhi_downsample PUBLIC cxx_std_17)
    target_link_libraries(gvdb_nvrhi_downsample PUBLIC nvrhi)
    target_include_directories(gvdb_nvrhi_downsample PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
        "$<INSTALL_INTERFACE:include/gvdb>")
    set_target_properties(gvdb_nvrhi_downsample PROPERTIES EXPORT_NAME nvrhi_downsample POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(gvdb_nvrhi_downsample PRIVATE /utf-8)
    endif()
    if(COMMAND ethereal_compile_shaders)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-downsample.cfg"
            CONTENT "\"${CMAKE_CURRENT_LIST_DIR}/slang/GVDBDenseDownsample.slang\" -T cs -E downsampleDenseRegion\n" @ONLY)
        ethereal_compile_shaders(TARGET gvdb_nvrhi_downsample_shaders SLANG CUDA ALL_PLATFORMS
            CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-downsample.cfg"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_downsample")
    endif()
endif()
