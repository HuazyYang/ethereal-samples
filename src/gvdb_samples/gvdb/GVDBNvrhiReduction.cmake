# Read-only scalar queries over authenticated retained volume snapshots.
if(NOT TARGET gvdb_nvrhi_reduction)
    include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiVolume.cmake")
    add_library(gvdb_nvrhi_reduction STATIC "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiReduction.cpp")
    add_library(gvdb::nvrhi_reduction ALIAS gvdb_nvrhi_reduction)
    target_compile_features(gvdb_nvrhi_reduction PUBLIC cxx_std_17)
    target_link_libraries(gvdb_nvrhi_reduction PUBLIC gvdb::nvrhi_volume)
    target_include_directories(gvdb_nvrhi_reduction PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>"
        "$<INSTALL_INTERFACE:include/gvdb>")
    set_target_properties(gvdb_nvrhi_reduction PROPERTIES EXPORT_NAME nvrhi_reduction POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(gvdb_nvrhi_reduction PRIVATE /utf-8)
    endif()
    if(BUILD_TESTING AND TARGET donut_app AND TARGET gvdb_nvrhi_owner_shaders
            AND COMMAND ethereal_compile_shaders)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-reduction.cfg"
            CONTENT "\"${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasReduction.slang\" -T cs -E reduceActiveColumns\n" @ONLY)
        ethereal_compile_shaders(TARGET gvdb_nvrhi_reduction_shaders SLANG CUDA ALL_PLATFORMS
            CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-reduction.cfg"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_reduction")
    endif()
endif()
