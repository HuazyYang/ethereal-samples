# Authenticated point gathers live in the same binary as PointSet and volume facades.
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApi.cmake")
get_target_property(_gvdb_gather_api_added gvdb_nvrhi_api GVDB_GATHER_API_ADDED)
if(NOT _gvdb_gather_api_added)
    set_property(TARGET gvdb_nvrhi_api PROPERTY GVDB_GATHER_API_ADDED TRUE)
    target_sources(gvdb_nvrhi_api PRIVATE "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGatherApi.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGatherApi.h"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGather.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGather.h"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApiInternal.h")
    if(BUILD_TESTING AND TARGET donut_app AND TARGET gvdb_nvrhi_owner_shaders
            AND COMMAND ethereal_compile_shaders)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-gather.cfg"
            CONTENT "\"${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasGather.slang\" -T cs -E gatherOwnedAtlas\n" @ONLY)
        ethereal_compile_shaders(TARGET gvdb_nvrhi_gather_shaders SLANG CUDA ALL_PLATFORMS
            CONFIG "${CMAKE_CURRENT_BINARY_DIR}/gvdb-owned-gather.cfg"
            OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb_gather")
    endif()
    if(GVDB_NVRHI_BUILD_API_TESTS)
        add_executable(gvdb_nvrhi_gather_api_abi_tests "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBNvrhiGatherApiAbiTests.cpp")
        target_link_libraries(gvdb_nvrhi_gather_api_abi_tests PRIVATE gvdb::nvrhi_api)
        if(MSVC)
            target_compile_options(gvdb_nvrhi_gather_api_abi_tests PRIVATE /utf-8)
        endif()
        add_test(NAME gvdb_nvrhi_gather_api_abi COMMAND gvdb_nvrhi_gather_api_abi_tests)
        set_tests_properties(gvdb_nvrhi_gather_api_abi PROPERTIES TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:gvdb_nvrhi_api>;PATH=path_list_prepend:$<TARGET_FILE_DIR:nvrhi_core>")
    endif()
    if(GVDB_NVRHI_INSTALL)
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGatherApi.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGatherApi.md" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasGather.slang" DESTINATION "${CMAKE_INSTALL_DATADIR}/gvdb/shaders")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiGatherApiTests.cmake")
unset(_gvdb_gather_api_added)
