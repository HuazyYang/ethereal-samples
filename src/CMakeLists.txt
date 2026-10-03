if (NVRHI_WITH_DX12)
	# The Agility SDK provides the D3D12 features the ray tracing, work graph and
	# async compute examples rely on; its DLLs are loaded from <exe dir>/D3D12/.
	# A stable release: preview releases (1.7xx-preview) refuse to load unless Windows Developer Mode is on
	# ("Preview releases of D3D12Core require Developer Mode"). Override for a preview, e.g. 1.717.1-preview.
	set(ETHEREAL_AGILITY_SDK_VERSION "1.619.6" CACHE STRING "Microsoft.Direct3D.D3D12 NuGet package version")
	set(DONUT_D3D_AGILITY_SDK_URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${ETHEREAL_AGILITY_SDK_VERSION}")
	set(DONUT_D3D_AGILITY_SDK_FETCH_DIR "" CACHE STRING "" FORCE)
	include("${ETHEREAL_DONUT_DIR}/cmake/FetchAgilitySDK.cmake")

	file(COPY
		"${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12Core.dll"
		"${DONUT_D3D_AGILITY_SDK_PATH}/build/native/bin/x64/D3D12SDKLayers.dll"
		DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/")
endif()

# Makes <target> load the Agility SDK D3D12 runtime from <exe dir>/D3D12/. Needed by every executable
# that creates ray tracing acceleration structures: nvrhi flags their buffers with a resource flag that
# older system D3D12 runtimes reject. (work_graphs exports the same symbols itself.)
function(ethereal_use_agility_sdk target)
	if (NVRHI_WITH_DX12 AND DONUT_D3D_AGILITY_SDK_VERSION)
		target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AgilitySDKExports.cpp")
		target_compile_definitions(${target} PRIVATE ETHEREAL_D3D12_SDK_VERSION=${DONUT_D3D_AGILITY_SDK_VERSION})
	endif()
endfunction()

# --- ethereal samples -----------------------------------------------------------
add_subdirectory(ddgi_samples)
add_subdirectory(vxgi_samples)
if (ETHEREAL_BUILD_GVDB)
	add_subdirectory(gvdb_samples)
endif()

# --- Donut examples (from Donut-Samples/examples) --------------------------------
add_subdirectory(basic_triangle)
add_subdirectory(vertex_buffer)
add_subdirectory(deferred_shading)
add_subdirectory(headless)

if (NVRHI_WITH_VULKAN OR NVRHI_WITH_DX12)
	add_subdirectory(bindless_rendering)
	add_subdirectory(variable_shading)
	add_subdirectory(rt_triangle)
	add_subdirectory(rt_shadows)
	add_subdirectory(rt_bindless)
	add_subdirectory(rt_particles)
	add_subdirectory(meshlets)
	add_subdirectory(threaded_rendering)
	add_subdirectory(async_compute)
endif()

if (NVRHI_WITH_DX12)
	# Work Graphs use a DX12-specific API
	add_subdirectory(work_graphs)

	# The RT Reflections example uses local bindings (local root signatures), which Vulkan lacks
	add_subdirectory(rt_reflections)
endif()

if (NVRHI_WITH_VULKAN)
	# Shader specializations are a unique Vulkan feature
	add_subdirectory(shader_specializations)
endif()

if (DONUT_WITH_AFTERMATH AND NVRHI_WITH_AFTERMATH)
	add_subdirectory(aftermath)
endif()
