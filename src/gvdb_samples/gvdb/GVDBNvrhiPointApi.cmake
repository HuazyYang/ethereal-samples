# Owned point conversion and transactional topology use the existing public API binary.
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiApi.cmake")
get_target_property(_gvdb_point_api_added gvdb_nvrhi_api GVDB_POINT_API_ADDED)
if(NOT _gvdb_point_api_added)
    set_property(TARGET gvdb_nvrhi_api PROPERTY GVDB_POINT_API_ADDED TRUE)
    target_sources(gvdb_nvrhi_api PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApi.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApi.h"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBPointStreamConversion.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/GVDBPointStreamConversion.h")
    if(GVDB_NVRHI_INSTALL)
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApi.h"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApi.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gvdb")
        install(FILES "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPointStreamConversion.slang"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/gvdb/shaders")
    endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiPointApiTests.cmake")
unset(_gvdb_point_api_added)
