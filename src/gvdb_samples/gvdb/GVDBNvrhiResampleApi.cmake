# Public resample facade shares the volume implementation's authenticated bridge.
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResample.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiApi.cmake")
get_target_property(_gvdb_resample_api_added gvdb_nvrhi_api GVDB_RESAMPLE_API_ADDED)
if(NOT _gvdb_resample_api_added)
    set_property(TARGET gvdb_nvrhi_api PROPERTY GVDB_RESAMPLE_API_ADDED TRUE)
    target_sources(gvdb_nvrhi_api PRIVATE "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResampleApi.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResampleApi.h")
    target_link_libraries(gvdb_nvrhi_api PRIVATE gvdb::nvrhi_resample)
    if(GVDB_NVRHI_BUILD_API_TESTS)
        add_executable(gvdb_nvrhi_resample_api_abi_tests "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBNvrhiResampleApiAbiTests.cpp")
        target_link_libraries(gvdb_nvrhi_resample_api_abi_tests PRIVATE gvdb::nvrhi_api)
        if(MSVC)
            target_compile_options(gvdb_nvrhi_resample_api_abi_tests PRIVATE /utf-8)
        endif()
        add_test(NAME gvdb_nvrhi_resample_api_abi COMMAND gvdb_nvrhi_resample_api_abi_tests)
        set_tests_properties(gvdb_nvrhi_resample_api_abi PROPERTIES TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:gvdb_nvrhi_api>;PATH=path_list_prepend:$<TARGET_FILE_DIR:nvrhi_core>")
    endif()
    if(GVDB_NVRHI_INSTALL)
        install(TARGETS gvdb_nvrhi_resample EXPORT gvdbNvrhiTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResampleApi.h"
            "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResample.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResampleApi.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasResample.slang"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/gvdb/shaders")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiResampleApiTests.cmake")
unset(_gvdb_resample_api_added)
