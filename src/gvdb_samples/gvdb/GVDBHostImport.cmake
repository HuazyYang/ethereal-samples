# Standalone CPU bridge from serialized pools to mutable host ownership.
if(NOT TARGET gvdb_host_import)
    include("${CMAKE_CURRENT_LIST_DIR}/GVDBHostVolume.cmake")
    add_library(gvdb_host_import STATIC
        "${CMAKE_CURRENT_LIST_DIR}/GVDBHostImport.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBHostImport.h")
    add_library(gvdb::host_import ALIAS gvdb_host_import)
    target_link_libraries(gvdb_host_import PUBLIC gvdb::host_volume)
    if(MSVC)
        target_compile_options(gvdb_host_import PRIVATE /utf-8)
    endif()
    if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
        add_executable(gvdb_host_import_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBHostImportTests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPortableData.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPortableVbx.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBTopologyBuilder.cpp")
        target_link_libraries(gvdb_host_import_tests PRIVATE gvdb::host_import)
        if(MSVC)
            target_compile_options(gvdb_host_import_tests PRIVATE /utf-8)
        endif()
        add_test(NAME gvdb_host_import COMMAND gvdb_host_import_tests
            "${CMAKE_CURRENT_LIST_DIR}/../samples/assets/explosion.vbx")
        set_tests_properties(gvdb_host_import PROPERTIES TIMEOUT 30)
    endif()
endif()
