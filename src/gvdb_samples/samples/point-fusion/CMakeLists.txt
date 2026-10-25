# gPointFusion: simulated depth scans of a procedural city (PointFusionScan.slang) fused
# incrementally into a level set (accumulateTopology + gatherLevelSet).
gvdb_add_sample(point-fusion SOURCES main.cpp KERNELS PointFusionScan.slang)
