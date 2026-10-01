
add_executable(mesh_voxelize
    MeshVoxelizePass.cpp
)
target_link_libraries(mesh_voxelize PRIVATE donut_engine donut_app gvdb sample_utils)
add_dependencies(mesh_voxelize sample_uitls_shaderlib sample_utils_ptx)

add_custom_command(
    TARGET mesh_voxelize
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:gvdbPTX> $<TARGET_FILE_DIR:mesh_voxelize>
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mesh_voxelize>/asset"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../assets/lucy.obj" "$<TARGET_FILE_DIR:mesh_voxelize>/asset/"
    COMMAND_EXPAND_LISTS
)

