# Demos -- reconstructions and ports of complete applications, as opposed to the
# single-feature samples in src/ and the micro-benchmarks in benchmark/.
#
#   nv_asteroids/   the 2018 NVIDIA Asteroids meshlet demo, reconstructed from
#                   its shipped binaries and ported to Donut/nvrhi ethereal-dev
if (NOT WIN32)
	message(STATUS "demos: skipped, Windows only")
	return()
endif()

add_subdirectory(nv_asteroids)
