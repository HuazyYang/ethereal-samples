set(project VXGISample)
set(folder "VXGISample")

file(GLOB sources "*.cpp" "*.h")
file(GLOB_RECURSE shaders "shaders/*.hlsl" "shaders/*.hlsli")

ethereal_compile_shaders(
    TARGET ${project}_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    SOURCES ${shaders}
    FOLDER ${folder}
    ALL_PLATFORMS
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} PRIVATE donut_engine donut_app donut_render)
add_dependencies(${project} ${project}_shaders)
target_compile_definitions(${project} PRIVATE ETHEREAL_ASSETS_DIR="${ETHEREAL_SAMPLES_ASSETS_DIR}")
set_target_properties(${project} PROPERTIES FOLDER ${folder})
