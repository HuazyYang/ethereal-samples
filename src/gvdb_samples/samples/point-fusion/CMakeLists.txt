# gPointFusion: simulated depth scans of a procedural city (PointFusionScan.cu) fused
# incrementally into a level set (accumulateTopology + gatherLevelSet).
gvdb_add_sample(point-fusion SOURCES main.cpp PTX PointFusionScan.cu)
