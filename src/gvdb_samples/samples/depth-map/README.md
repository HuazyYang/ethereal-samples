# Depth-map

Lucy mesh rasterization, sparse explosion-volume ray marching clipped by raster depth, and graphics composition. All three stages are Slang artifacts loaded through NVRHI. The default uses CUDA compute when that backend is built; `--compute-api graphics` keeps computation on the selected graphics device. The legacy implementation remains in `legacy-main.cpp` and is selected when the Slang target is disabled.

```powershell
depth-map -dx12 --compute-api cuda
depth-map -vk --compute-api graphics
depth-map -dx11 --compute-api cuda --headless --width 160 --height 120 --output depth.png
depth-map -dx12 --compute-api cuda --self-test --validation
```

Select the graphics API with `-dx11`, `-dx12` or `-vk`. Select CUDA independently with `--cuda-device N`; the application matches a graphics adapter by public UUID/LUID unless `--graphics-adapter N` is specified. Unsupported or mismatched devices fail explicitly. No frame transfer falls back to host staging.

Mouse controls preserve the original orbit, pan and distance gestures; Shift adjusts the light. WASDQE translates the volume, arrow keys rotate it, P switches depth projection mode, and brackets adjust exact-mode bias. `--frames N --screenshot image.png` captures and closes an interactive run. `--headless` validates the actual Lucy/explosion scene, including nonzero mesh and volume coverage and a measurable depth-clipping effect. `--volume` and `--mesh` override asset paths.

Volume loading uses the public serializer and retains its immutable snapshot after the temporary owner and serializer are released. The clipping shader binds that snapshot's canonical raw R32 atlas and topology ranges. The default apron refresh uses the public voxel operator, recorded after import on the same command list; callers setting `Scene::refreshApron=false` retain the imported apron bytes. Rasterization, transfer lookup and clipping equations are unchanged. Production remap and apron artifacts are built even with `BUILD_TESTING=OFF`; the old structured-atlas path exists only in the comparison test target.

The CUDA route shares an R32_FLOAT raster color attachment containing projected depth and an R32_UINT output texture containing packed RGBA8. The kernel loads the imported depth texture directly. The public route supplies each API's required resource flags, including unkeyed NT sharing on D3D11. Shared GPU timeline waits/signals and public whole-resource release/acquire calls order raster, CUDA and composition; normal frames use no CPU wait or readback. Allocation, resize, asset replacement and requested screenshots may wait. The code contains no CUDA, DXGI, Direct3D or Vulkan SDK calls.

Default `--depth-mode legacy` preserves the original view-Z depth approximation, transfer lookup, sampling wavefront, byte quantization and double-alpha composition. The old depth-bias slider was unused by the volume kernel; bias remains inactive in compatibility mode. `--depth-mode exact` accounts for the off-axis perspective factor and supports `--bias N` in index-space ray units. Neither mode changes the original volume transform or camera defaults.

`--self-test` uses an independently evaluated constant-density sparse volume behind rasterized planes. Cases cover near/far clipping, empty depth, off-axis projection, nonuniform transformed instances, legacy bias behavior, repeated GPU ownership cycles, image row orientation, resource resize, and composition quantization. CTest registers analytic and real-asset runs for each enabled graphics API and compute mode. Native validation treats warnings/errors as failures. CUDA Compute Sanitizer can wrap the CUDA-mode runs.

The implementation contract and its reference behavior are archived in the aggregate's `docs/plans/2026-10-18-depth-map-slang-migration.md`. Visual parity against captured legacy screenshots is a separate acceptance check; passing a constant-volume fixture alone does not establish parity with the explosion image.

Validated Windows/RTX 4050: all 12 CTests pass, plus native validation and CUDA memory checking on all three graphics/CUDA routes (zero reported errors). At 1024x768 the preserved legacy DX12/CUDA screenshot and migrated default image differ by a mean 0.005712 RGB levels (0-255); 0.157% of pixels differ by more than one level. The comparison is tolerant, with a maximum channel difference 57.

The owned-volume migration adds `gvdb_depth_owned_tests`. Its independent projected-plane expectations remain, and its separate structured-atlas shader checks exact packed output for every analytic case and the real Lucy/explosion scene. A poisoned-apron fixture verifies both preservation and sparse-boundary reconstruction and requires those two images to differ. All 24 depth-map CTests pass in the full build, as do all 12 in the CUDA-disabled graphics build. Native diagnostics are clean on all six routes and CUDA memcheck reports zero errors on all three graphics/CUDA routes. At 160x120, the new production executable's PNG is byte-identical to the preserved pre-ownership executable on all six routes. The tests-off, CUDA-toolkit-disabled production build also runs the D3D11 analytic and real-asset checks successfully.
