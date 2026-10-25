# gvdb library: the GVDB data structure with its operation objects and
# scene-graph integration, on the nvrhi CUDA backend (GVDBCompute.h).
# Headers sit next to their sources and are included as <gvdb/...>.

# --- kernels (Slang, NVRHI shader containers) ---------------------------------
# shaders/*.slang -> bin/shaders/gvdb/*.nvsc, read through the file system of
# the compute context (GVDB_SHADERS_* in GVDB.h).

ethereal_compile_shader_containers(
    TARGET gvdb_shaders
    SOURCES
        "shaders/GVDBOperators.slang"
        "shaders/GVDBParticles.slang"
        "shaders/GVDBPrimitives.slang"
    TARGETS ptx
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/gvdb"
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/shaders"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/shaders"
    EXTRA_FLAGS ${GVDB_SHADER_FLAGS}
    FOLDER "GVDBSamples/shaders")

# --- gvdb host library --------------------------------------------------------

add_library(gvdb STATIC
    GVDB.h
    GVDBCompute.h
    GVDBCompute.cpp
    GVDBPrimitives.h
    GVDBPrimitives.cpp
    GVDBTypes.cpp
    GVDBAllocator.h
    GVDBAllocator.cpp
    GVDBVolume.cpp
    GVDBVoxelOps.cpp
    GVDBPointOps.cpp
    GVDBVoxelizer.cpp
    GVDBSerializer.cpp
    GVDBScene.h
    GVDBScene.cpp
    shaders/GVDB.slangh
    shaders/GVDBOperators.slang
    shaders/GVDBParticles.slang
    shaders/GVDBPrimitives.slang
)
set_source_files_properties(
    shaders/GVDB.slangh shaders/GVDBOperators.slang shaders/GVDBParticles.slang shaders/GVDBPrimitives.slang
    PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(gvdb
    PUBLIC "$<BUILD_INTERFACE:${GVDB_INCLUDE_ROOT}>"
           "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(gvdb PRIVATE "NOMINMAX=1")
# PUBLIC donut_engine: <gvdb/GVDBScene.h> derives from the Donut scene graph;
# nvrhi: <gvdb/GVDBCompute.h> works on the nvrhi CUDA device.
target_link_libraries(gvdb
    PUBLIC donut_core donut_engine nvrhi
)
if (TARGET gvdb_shaders)
    add_dependencies(gvdb gvdb_shaders)
endif()
set_target_properties(gvdb PROPERTIES FOLDER "GVDBSamples")

# --- tests ----------------------------------------------------------------------

add_executable(gvdb_tests tests/gvdb-tests.cpp)
target_link_libraries(gvdb_tests PRIVATE gvdb donut_app)
if (TARGET nvrhi_factory)
    target_link_libraries(gvdb_tests PRIVATE nvrhi_factory)
endif()
target_compile_definitions(gvdb_tests PRIVATE "NOMINMAX=1")
set_target_properties(gvdb_tests PROPERTIES FOLDER "GVDBSamples")
foreach(gvdb_test primitives volume)
    add_test(NAME gvdb_${gvdb_test} COMMAND gvdb_tests ${gvdb_test})
endforeach()
