# GVDB samples

The GVDB samples use the portable Slang/NVRHI path. The host topology and volume
owners live in `gvdb/GVDBHostVolume.*` and `gvdb/GVDBNvrhi*`; Slang kernels and
their manifests are under `gvdb/slang`. Applications and focused fixtures are
under `samples/` and `sph_fluid_pool/`.

The migrated targets are added by `src/slang_compute/CMakeLists.txt`. They use
offline Slang artifacts and NVRHI resources on D3D11, D3D12, Vulkan and CUDA
where the corresponding backend is enabled. Host topology and VBX tests can be
built without CUDA toolkit or NVCC discovery.

The former device layer, affine GVDB scene/volume implementation, handwritten
CUDA kernels and legacy sample launchers have been removed. CUDA
execution uses the NVRHI backend and the Slang kernel manifests directly.
