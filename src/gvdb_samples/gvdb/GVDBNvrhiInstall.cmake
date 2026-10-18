# Optional SDK packaging. Kept separate from the CPU-only/core ownership modules.
include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET gvdb_nvrhi_api)
    message(FATAL_ERROR "Include GVDBNvrhiApi.cmake before GVDBNvrhiInstall.cmake")
endif()
# Preserve every source-tree include for builds, but never serialize source paths
# into the installed SDK. NVRHI is found independently by the package config.
foreach(_gvdb_target gvdb_host_volume gvdb_nvrhi_volume)
    get_target_property(_gvdb_source_includes ${_gvdb_target} INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET ${_gvdb_target} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    foreach(_gvdb_include IN LISTS _gvdb_source_includes)
        target_include_directories(${_gvdb_target} INTERFACE "$<BUILD_INTERFACE:${_gvdb_include}>")
    endforeach()
    target_include_directories(${_gvdb_target} INTERFACE
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gvdb>"
        "$<INSTALL_INTERFACE:$<TARGET_PROPERTY:nvrhi,INTERFACE_INCLUDE_DIRECTORIES>>")
endforeach()
set_target_properties(gvdb_host_volume PROPERTIES EXPORT_NAME host_volume)
set_target_properties(gvdb_nvrhi_volume PROPERTIES EXPORT_NAME nvrhi_volume)
# Use the selected installation header directory, including nondefault layouts.
set_property(TARGET gvdb_nvrhi_api PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gvdb>")
install(TARGETS gvdb_host_volume gvdb_nvrhi_volume gvdb_nvrhi_api EXPORT gvdbNvrhiTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiApi.h"
    "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhi.h"
    "${CMAKE_CURRENT_LIST_DIR}/GVDBHostVolume.h"
    "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiVolume.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiApi.md" "${CMAKE_CURRENT_LIST_DIR}/GVDBNvrhiVolume.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gvdb")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBPortable.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gvdb/slang")
# Sources are optional offline-build inputs, never loaded or compiled by the API.
install(FILES "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBAtlasRepack.slang"
    "${CMAKE_CURRENT_LIST_DIR}/slang/GVDBTypes.slang"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/gvdb/shaders")
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/gvdbNvrhiConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/gvdbNvrhiConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gvdbNvrhi")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/gvdbNvrhiConfigVersion.cmake"
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(EXPORT gvdbNvrhiTargets NAMESPACE gvdb:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gvdbNvrhi")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gvdbNvrhiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/gvdbNvrhiConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gvdbNvrhi")
unset(_gvdb_target)
unset(_gvdb_source_includes)
unset(_gvdb_include)
