# ---------------------------------------------------------------------------
# GVDBSamples -- the gvdb sparse-voxel-database library and its samples.
#
#   gvdb/     the library: public headers, implementation, and the gpdevice
#             compute-abstraction layer it is built on
#   samples/  the samples and their assets
#
# Migrated from the lithereal project and linked against donut rather than the
# old "ethereal" fork of donut.
# ---------------------------------------------------------------------------

find_package(CUDAToolkit REQUIRED)

add_subdirectory(gvdb)
add_subdirectory(samples)
