# Add serialization to the same binary as authenticated public snapshot/volume wrappers.
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiApi.cmake")
get_target_property(_gvdb_serializer_added gvdb_nvrhi_api GVDB_SERIALIZER_ADDED)
if(NOT _gvdb_serializer_added)
    set_property(TARGET gvdb_nvrhi_api PROPERTY GVDB_SERIALIZER_ADDED TRUE)
    target_sources(gvdb_nvrhi_api PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiSerializer.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiSerializer.h"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBHostImport.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPortableData.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPortableVbx.cpp")
    if(GVDB_NVRHI_BUILD_API_TESTS)
        add_executable(gvdb_nvrhi_serializer_abi_tests "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBNvrhiSerializerAbiTests.cpp")
        target_link_libraries(gvdb_nvrhi_serializer_abi_tests PRIVATE gvdb::nvrhi_api)
        if(MSVC)
            target_compile_options(gvdb_nvrhi_serializer_abi_tests PRIVATE /utf-8)
        endif()
        add_test(NAME gvdb_nvrhi_serializer_abi COMMAND gvdb_nvrhi_serializer_abi_tests)
        set_tests_properties(gvdb_nvrhi_serializer_abi PROPERTIES TIMEOUT 30
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:gvdb_nvrhi_api>;PATH=path_list_prepend:$<TARGET_FILE_DIR:nvrhi_core>")
    endif()
    if(GVDB_NVRHI_INSTALL)
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiSerializer.h"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiSerializer.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gvdb")
    endif()
endif()
unset(_gvdb_serializer_added)
