# NOTE(migration): lithereal overrode CMAKE_RUNTIME_OUTPUT_DIRECTORY here with a
# per-config "${CMAKE_BINARY_DIR}/bin/$<CONFIG>" layout. That override was dropped so
# these samples follow this project's convention (the root CMakeLists puts every
# runtime artifact in "${CMAKE_BINARY_DIR}/bin").

add_subdirectory(sample_utils)

# TODO(migration): mesh_voxelize and depth_map do NOT compile and are disabled.
# This is a pre-existing breakage in the lithereal source snapshot, not an
# ethereal->donut mapping problem:
#   * MeshVoxelizePass.cpp references gvdb::GVDBScene / GVDBModel / GVDBCamera /
#     GVDBLight / GVDBShadeType, which exist nowhere in lithereal; the real types
#     are SampleUtils::Scene / Model / Camera / Light and SampleUtils::VolShadeType.
#   * DepthMapPass.cpp additionally includes <sample_utils/AppUtils.h>,
#     <gvdb/GVDBVoxelizer.h>, <gvdb/GVDBRenderer.h>, <gvdb/GVDBScene.h> and
#     <gvdb/GVDBCamera.h>, none of which exist in the lithereal repository.
# Re-enable these once the sample sources are ported onto the SampleUtils API.
# add_subdirectory(mesh_voxelize)
# add_subdirectory(depth_map)
