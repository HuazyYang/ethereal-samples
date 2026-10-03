# gPointCloud: a point time series (points.dat) gathered into a level set, polygon models from a
# scene file (*.scn), rendered progressively with OptiX (CUDA raycaster as a fallback).
gvdb_add_sample(point-cloud OPTIX SOURCES main.cpp SceneFile.h SceneFile.cpp)
