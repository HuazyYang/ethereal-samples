# hello_dxr: Donut / nvrhi port of D3D12-Trial/HelloDXR (ray traced tetrahedra, plane and Menger
# sponge with shadow rays; the TLAS is refitted every frame). D3D12 and Vulkan.

file(GLOB shaders "*.hlsl")
file(GLOB sources "*.cpp" "*.h")

set(project hello_dxr)
set(folder "Examples/Hello DXR")

ethereal_compile_shaders(
    TARGET ${project}_shaders
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
    SOURCES ${shaders}
    FOLDER ${folder}
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_render donut_app donut_engine)
add_dependencies(${project} ${project}_shaders)
ethereal_use_agility_sdk(${project})
set_target_properties(${project} PROPERTIES FOLDER ${folder})
