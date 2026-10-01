
add_executable(depth_map
    DepthMapPass.cpp
)
target_link_libraries(depth_map PRIVATE donut_engine donut_app gvdb sample_utils)
add_dependencies(depth_map gvdbPTX sample_uitls_shaderlib)

add_custom_command(
    TARGET depth_map
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:gvdbPTX> $<TARGET_FILE_DIR:depth_map>
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:depth_map>/asset"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../assets/lucy.obj" "$<TARGET_FILE_DIR:depth_map>/asset/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../assets/explosion.vbx" "$<TARGET_FILE_DIR:depth_map>/asset/"
    COMMAND_EXPAND_LISTS
    COMMENT "Copy depth_map runtime dependency files..."
)

