# CPU-overhead benchmarks. Windows only (the native baselines are D3D11/D3D12).
if (NOT WIN32)
	message(STATUS "benchmark: the renderers are skipped, Windows only (the analysis tests still run)")
	# The statistics of scripts/analyze.py are plain Python and do not depend on the platform.
	find_package(Python3 COMPONENTS Interpreter)
	if (Python3_FOUND)
		add_test(NAME benchmark_analysis
			COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/Asteroids/tests/analysis")
	endif()
	return()
endif()

add_subdirectory(Asteroids)
