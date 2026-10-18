# Additive M5 CPU ownership draft. This module performs no shader/SDK discovery.
# Standalone users can set GVDB_NVRHI_INCLUDE_DIR instead of defining target nvrhi.
if(NOT TARGET gvdb_host_volume)
    if(TARGET nvrhi)
        set(_gvdb_host_nvrhi_headers "$<TARGET_PROPERTY:nvrhi,INTERFACE_INCLUDE_DIRECTORIES>")
    elseif(GVDB_NVRHI_INCLUDE_DIR AND EXISTS "${GVDB_NVRHI_INCLUDE_DIR}/nvrhi/nvrhi.h")
        set(_gvdb_host_nvrhi_headers "${GVDB_NVRHI_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "GVDBHostVolume requires NVRHI headers: define target nvrhi or GVDB_NVRHI_INCLUDE_DIR")
    endif()
    add_library(gvdb_host_volume STATIC
        "${CMAKE_CURRENT_LIST_DIR}/GVDBHostVolume.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBHostVolume.h"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhi.h")
    add_library(gvdb::host_volume ALIAS gvdb_host_volume)
    target_compile_features(gvdb_host_volume PUBLIC cxx_std_17)
    target_include_directories(gvdb_host_volume PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}" "${_gvdb_host_nvrhi_headers}")
    if(MSVC)
        target_compile_options(gvdb_host_volume PRIVATE /utf-8)
    endif()

    if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
        add_executable(gvdb_host_volume_tests "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBHostVolumeTests.cpp")
        target_link_libraries(gvdb_host_volume_tests PRIVATE gvdb::host_volume)
        add_executable(gvdb_nvrhi_contract_tests "${CMAKE_CURRENT_LIST_DIR}/tests/GVDBNvrhiContractTests.cpp")
        # The descriptor mock constructs BufferDesc::debugName and needs only
        # the CPU allocator, not an NVRHI device/backend implementation.
        if(TARGET nvrhi_core)
            target_link_libraries(gvdb_nvrhi_contract_tests PRIVATE nvrhi_core)
        elseif(EXISTS "${GVDB_NVRHI_INCLUDE_DIR}/../src/core/memory.cpp")
            target_sources(gvdb_nvrhi_contract_tests PRIVATE "${GVDB_NVRHI_INCLUDE_DIR}/../src/core/memory.cpp")
        else()
            message(FATAL_ERROR "CPU descriptor fixture requires nvrhi_core or an NVRHI source-tree include directory")
        endif()
        target_compile_features(gvdb_nvrhi_contract_tests PRIVATE cxx_std_17)
        target_include_directories(gvdb_nvrhi_contract_tests PRIVATE "${_gvdb_host_nvrhi_headers}")
        if(MSVC)
            target_compile_options(gvdb_host_volume_tests PRIVATE /utf-8)
            target_compile_options(gvdb_nvrhi_contract_tests PRIVATE /utf-8)
        endif()
        add_test(NAME gvdb_host_volume COMMAND gvdb_host_volume_tests)
        add_test(NAME gvdb_nvrhi_contract COMMAND gvdb_nvrhi_contract_tests)
        set_tests_properties(gvdb_host_volume gvdb_nvrhi_contract PROPERTIES TIMEOUT 30)
        if(TARGET nvrhi_core)
            set_tests_properties(gvdb_nvrhi_contract PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:nvrhi_core>")
        endif()
    endif()
    unset(_gvdb_host_nvrhi_headers)
endif()
