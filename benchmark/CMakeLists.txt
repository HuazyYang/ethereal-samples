# CPU-overhead benchmarks. Windows only (the native baselines are D3D11/D3D12).
if (NOT WIN32)
	message(STATUS "benchmark: skipped, Windows only")
	return()
endif()

add_subdirectory(Asteroids)
