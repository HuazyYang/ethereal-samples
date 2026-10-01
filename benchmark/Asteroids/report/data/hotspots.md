# Hot-spot attribution: where the per-draw CPU time goes

CPU sampling profiles of the headline configurations (RTX 4050, 1080 x 720, 50,001 draws per frame), one 20 s
capture per configuration (two populations for nvrhi D3D12 `tex_mut`), 4 kHz kernel sampling with stacks through
VSDiagnostics (the shell was not elevated, so `wpr` was not available), symbolised with xperf and dbghelp. Procedure:
`docs/PROFILING.md`; every table of every configuration: `report/data/hotspots_tables.md`; numbers:
`report/data/hotspots.json`; raw captures: `results/profiles-01/` (git-ignored).

Each statement below is marked **[measured]** (read from the samples and stacks of these captures) or
**[inferred]** (an explanation of a measured number from the source code or from reasoning, not itself measured).

## 0. How to read the numbers

- All numbers are **main-thread samples inside render + submit**, classified by the source line of the renderer's
  `Render` frame against the benchmark's `lap.To` boundaries (nvrhi: the whole `AsteroidsRenderer::Render`). At 1
  thread the samples reproduce the run's `median_cpu_render_ms` to within 2 % (0.241-0.257 ms per sample against the
  nominal 0.25), so the classification is sound [measured].
- **Profiled runs are not the measured numbers.** The 4 kHz sampler slows the profiled process by 1 % (native) to
  18 % (nvrhi) and 34 % (Diligent Vulkan); `profiled` columns are `share x median_cpu_render_ms of the profiled run /
  50,001`, `scaled` columns are `share x measured median of summary.csv / 50,001`. Comparisons in the text use the
  scaled numbers unless stated; the shares are the primary result.
- **Charged** = a sample is charged to the first frame from the leaf upward that is application code, the layer
  (`nvrhi::*` / `Diligent::*`), the API runtime (`D3D12Core.dll`, `d3d12.dll`, `dxgi.dll`, `vulkan-1.dll`) or the
  driver (`nvwgf2umx.dll`, `nvoglv64.dll`, `nvlddmkm.sys`); OS and CRT time (kernel, `ntdll`, `vcruntime140`
  memcpy/memset, `std::` containers inside the image) goes to the caller. "Driver" therefore means *driver code called
  by that path*, and "layer own" means the layer's own instructions plus its memcpy/hash-map/CRT work.
- The NVIDIA user-mode drivers have no public symbols: driver cost is attributed through the runtime entry point
  (`SetGraphicsRootConstantBufferView`, `DrawIndexedInstanced`, ...) on D3D12 and through the nvrhi/Diligent function
  that called `vkCmd*` on Vulkan (the Vulkan loader trampolines are statically dispatched into `nvoglv64.dll`).
- `/GL` + `/LTCG` inlines; `/OPT:ICF` folds identical functions. `nvrhi::d3d12::Sampler::getDesc` /
  `nvrhi::d3d11::Sampler::getDesc` in the tables are the folded `IBindingSet::getDesc` virtual calls, Diligent's
  `EngineFactoryBase::GetReferenceCounters` under `MapBuffer` is another folded trivial getter [inferred from identical
  code, measured as the symbol the linker kept].
- 8 threads: the main thread yields (`SwitchToThread`) while it waits for the workers and is then not sampled; its
  samples are converted at 0.25 ms and the remainder of `median_cpu_render_ms` is reported as off-CPU. Worker numbers
  are one worker's samples in its recording function per frame, divided by its 6,250 draws. All renderers' per-draw
  costs on a worker are 1.6-2.1 x their 1-thread value (8 recording threads on 8 cores / 16 SMT threads, lower
  all-core clocks) [measured]; compare workers with workers.

## 1. Overview: render + submit per draw by group (main thread, ns/draw scaled to the measured median)

| configuration | measured ms (summary.csv) | profiled ms | app | layer own | runtime | driver | wait for workers | off-CPU | total ns/draw |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| native D3D12, 1 thread | 0.892 | 0.901 | 1.2 | - | 2.5 | 14.1 | - | - | 17.8 |
| Diligent D3D12 tex_mut, 1 thread | 3.929 | 4.211 | 2.7 | 54.4 | 2.9 | 18.5 | - | - | 78.6 |
| nvrhi D3D12 tex_mut, 1 thread, no page split | 5.838 | 6.904 | 2.9 | 94.0 | 3.1 | 16.8 | - | - | 116.8 |
| nvrhi D3D12 tex_mut, 1 thread, page split forced | 10.615 | 11.319 | 5.2 | 187.6 | 3.7 | 15.8 | - | - | 212.3 |
| nvrhi D3D12 tex_mut_pc, 1 thread, no page split | 5.820 | 6.703 | 5.4 | 62.1 | 3.9 | 44.9 | - | - | 116.4 |
| nvrhi D3D12 bindless, 1 thread | 1.137 | 1.259 | 6.5 | 5.9 | 1.7 | 8.6 | - | - | 22.7 |
| Diligent D3D12 bindless, 1 thread | 1.369 | 1.562 | 11.8 | 7.9 | 1.3 | 6.4 | - | - | 27.4 |
| native D3D12, 8 threads (main thread) | 0.482 | 0.466 | 0.4 | - | 1.0 | 5.3 | 0.7 | 2.4 | 9.8 |
| Diligent D3D12 tex_mut, 8 threads (main thread) | 1.093 | 1.256 | 0.8 | 8.2 | 1.1 | 6.3 | 3.3 | 2.2 | 21.9 |
| nvrhi D3D12 tex_mut, 8 threads, no page split (main thread) | 1.474 | 1.629 | 0.5 | 18.9 | 0.8 | 3.8 | 2.5 | 2.9 | 29.4 |
| Diligent Vulkan tex_mut, 1 thread | 3.798 | 5.074 | 4.5 | 35.1 | - | 36.3 | - | - | 76.0 |
| nvrhi Vulkan tex_mut, 1 thread | 11.492 | 13.577 | 7.5 | 136.7 + 3.0 Donut | - | 82.7 | - | - | 229.8 |
| nvrhi Vulkan tex_mut_pc, 1 thread | 6.793 | 7.903 | 3.4 | 84.0 | - | 48.4 | - | - | 135.9 |
| nvrhi Vulkan bindless, 1 thread | 1.640 | 2.028 | 7.8 | 4.3 | - | 20.7 | - | - | 32.8 |
| Diligent Vulkan bindless, 1 thread | 2.081 | 2.286 | 11.2 | 9.7 | - | 20.7 | - | - | 41.6 |

Shares behind this table (main thread, render + submit samples) [measured]:

| configuration | app | layer own | runtime | driver | wait | samples |
|---|---:|---:|---:|---:|---:|---:|
| native D3D12 t1 | 6.9 % | - | 14.0 % | 79.1 % | - | 25,904 |
| Diligent D3D12 tex_mut t1 | 3.5 % | 69.3 % | 3.7 % | 23.6 % | - | 53,867 |
| nvrhi D3D12 tex_mut t1 avoid | 2.4 % | 80.5 % | 2.7 % | 14.4 % | - | 61,099 |
| nvrhi D3D12 tex_mut t1 force | 2.4 % | 88.4 % | 1.7 % | 7.5 % | - | 66,919 |
| nvrhi D3D12 tex_mut_pc t1 avoid | 4.6 % | 53.4 % | 3.4 % | 38.6 % | - | 60,470 |
| nvrhi D3D12 bindless t1 | 28.5 % | 26.1 % | 7.6 % | 37.7 % | - | 29,535 |
| Diligent D3D12 bindless t1 | 43.1 % | 28.9 % | 4.6 % | 23.4 % | - | 33,835 |
| native D3D12 t8 (main) | 5.2 % | - | 13.1 % | 72.2 % | 9.5 % | 24,679 |
| Diligent D3D12 tex_mut t8 (main) | 4.0 % | 41.9 % | 5.5 % | 31.9 % | 16.8 % | 47,837 |
| nvrhi D3D12 tex_mut t8 avoid (main) | 1.9 % | 71.2 % | 2.8 % | 14.4 % | 9.6 % | 50,997 |
| Diligent Vulkan tex_mut t1 | 6.0 % | 46.2 % | - | 47.8 % | - | 54,747 |
| nvrhi Vulkan tex_mut t1 | 3.3 % | 59.5 % (+1.3 % Donut) | - | 36.0 % | - | 68,131 |
| nvrhi Vulkan tex_mut_pc t1 | 2.5 % | 61.8 % | - | 35.6 % | - | 63,465 |
| nvrhi Vulkan bindless t1 | 23.7 % | 13.2 % | - | 63.1 % | - | 41,386 |
| Diligent Vulkan bindless t1 | 27.0 % | 23.4 % | - | 49.6 % | - | 44,479 |

Module-level shares (leaf module, exclusive) for every configuration are in `hotspots_tables.md`; the pattern is the
same everywhere: the application image (`asteroids_*.exe`, `GraphicsEngine*_64r.dll`) plus `vcruntime140.dll`
(memcpy/memset called by the layer) carry the layer share, `nvwgf2umx.dll` / `nvoglv64.dll` the driver share,
`D3D12Core.dll` the runtime share; `ntdll`, `ntoskrnl`, `dxgkrnl`, `nvlddmkm.sys` together are below 2 % in every
1-thread configuration and sit under `ExecuteCommandLists` / `vkQueueSubmit` [measured].

## 2. Per configuration: entry points and top functions

Entry point = the outermost layer call below `Render` (for native: the outermost D3D12 runtime call); the rows
partition the render + submit samples. ns/draw are **profiled** (not scaled) here; multiply by
`measured / profiled` of section 1 to scale.

### 2.1 D3D12, 1 thread

**native D3D12** (18.0 ns/draw profiled) [measured]

| entry point | share | ns/draw | runtime | driver | app |
|---|---:|---:|---:|---:|---:|
| `SetGraphicsRootConstantBufferView` | 47.5 % | 8.6 | 1.3 | 7.3 | |
| `DrawIndexedInstanced` | 32.5 % | 5.9 | 0.6 | 5.2 | |
| `RenderSubset` own (matrix stores into the upload heap, loop) | 6.6 % | 1.2 | | | 1.2 |
| `ExecuteCommandLists` | 5.5 % | 1.0 | 0.2 | 0.8 | |
| `Close` + `Reset` + allocator `Reset` | 5.1 % | 1.0 | 0.2 | 0.7 | |

**Diligent D3D12 tex_mut** (84.2 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | runtime | driver |
|---|---:|---:|---:|---:|---:|
| `DeviceContextD3D12Impl::DrawIndexed` | 65.0 % | 54.7 | 34.8 | 2.3 | 17.6 |
| `DeviceContextD3D12Impl::MapBuffer` (DISCARD per draw) | 26.9 % | 22.7 | 22.6 | 0.1 | |
| `RenderSubset` own | 3.4 % | 2.9 | | | |
| `Flush` (explicit submit) | 3.1 % | 2.7 | 0.3 | 0.7 | 1.7 |
| `CommitShaderResources` | 0.4 % | 0.4 | 0.4 | | |

Top layer functions (inclusive / exclusive ns/draw): `DrawIndexed` 54.7 / 3.2, `CommitRootTablesAndViews<0>` 45.0 /
3.4, `PipelineResourceSignatureD3D12Impl::CommitRootTables` 30.3 / 24.5, `MapBuffer` 22.7 / 17.1,
`CommitRootViews` 11.2 / 2.1, `D3D12DynamicHeap::Allocate` 3.9 / 3.6. Hot lines: `CommitRootTables`
`PipelineResourceSignatureD3D12Impl.cpp:678` (the `SetDescriptorHeaps` check right after the loads from the SRB's
resource cache, 15.7 ns/draw across its inline chain) and `:637` (`GetParameterGroupSize`, 4.0), `MapBuffer`
`DeviceContextD3D12Impl.cpp:1784` (`AllocateDynamicSpace`, 9.1), `:1735` (3.1), `:1801` (1.7).

**nvrhi D3D12 tex_mut, no page split** (138.1 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | runtime | driver |
|---|---:|---:|---:|---:|---:|
| `CommandList::setGraphicsState` (called on the 90 % of draws whose texture changes) | 66.1 % | 91.3 | 79.0 | 1.8 | 10.5 |
| `CommandList::writeBuffer` (volatile constant buffer, per draw) | 18.3 % | 25.3 | 25.3 | | |
| `CommandList::drawIndexed` | 10.4 % | 14.4 | 6.7 | 0.9 | 6.8 |
| `RecordAsteroids` own | 2.4 % | 3.3 | | | |
| `Device::executeCommandLists` | 1.2 % | 1.7 | 0.1 | 0.5 | 1.1 |
| `open` + `close` + clears | 1.4 % | 1.9 | 0.1 | 0.3 | 1.4 |

Top layer functions (inclusive / exclusive ns/draw): `setGraphicsState` 91.3 / 19.9, `setGraphicsBindings` 64.9 /
32.0, `writeBuffer` 25.3 / 7.1, `drawIndexed` 14.4 / 0.8, `updateGraphicsVolatileBuffers` 6.7 / 1.2,
`UploadManager::suballocateBuffer` 6.7 / 6.6, `commitDescriptorHeaps` 2.4 / 1.7, `commitBarriers` 2.2 / 1.7,
`setResourceStatesForBindingSet` 2.1 / 1.1; besides these, `std::_Hash<IBuffer*, uint64>::_Try_emplace` (the
`m_VolatileConstantBufferAddresses` map) 14.9 exclusive and `vcruntime140` memcpy/memset 15.9 exclusive, both
charged to the layer. Hot lines: `d3d12-resource-bindings.cpp:1135` (`newVolatileCBs.push_back`, 12.2),
`d3d12-graphics.cpp:510` (`m_CurrentGraphicsState = state`, 9.4), `d3d12-resource-bindings.cpp:1185`
(`m_CurrentGraphicsVolatileCBs = newVolatileCBs`, 8.7), the three `unordered_map` lookups (`d3d12-buffer.cpp:572`
6.4, `d3d12-graphics.cpp:535` 4.6, `d3d12-resource-bindings.cpp:1118` 3.6), `d3d12-buffer.cpp:568` (the 160-byte
memcpy into write-combined upload memory, 5.0), `:1086` (`static_vector` constructor memset, 4.4), the folded
`getDesc` virtual calls (`:1100`, `d3d12-state-tracking.cpp:32`; 2.9).

**nvrhi D3D12 tex_mut, page split forced** (226.4 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | runtime | driver |
|---|---:|---:|---:|---:|---:|
| `setGraphicsState` | 75.7 % | 171.4 | 162.4 | 1.3 | 7.7 |
| `writeBuffer` | 11.6 % | 26.3 | 26.3 | | |
| `drawIndexed` | 7.6 % | 17.2 | 8.5 | 0.9 | 7.8 |
| `RecordAsteroids` own | 2.4 % | 5.5 | | | |
| rest | 2.7 % | 6.0 | | | |

Hot lines: `d3d12-graphics.cpp:510` **83.6 ns/draw** (36.9 % of all render + submit samples; 9.4 in the unaffected
run), `d3d12-resource-bindings.cpp:1185` 17.0 (8.7 unaffected), `:1135` 15.0 (12.2 unaffected). The difference
between the two populations, 88 ns/draw, is 74 at line 510 and 8 at line 1185 [measured].

**nvrhi D3D12 tex_mut_pc, no page split** (134.1 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | runtime | driver |
|---|---:|---:|---:|---:|---:|
| `setGraphicsState` | 54.9 % | 73.6 | 66.7 | 1.3 | 5.6 |
| `setPushConstants` (`SetGraphicsRoot32BitConstants`, 160 bytes per draw) | 27.2 % | 36.5 | 2.4 | 1.2 | 32.9 |
| `drawIndexed` | 10.3 % | 13.8 | 2.2 | 1.1 | 10.5 |
| `RecordAsteroids` own | 4.6 % | 6.1 | | | |
| rest | 3.0 % | 4.1 | | | |

**nvrhi D3D12 bindless** (25.2 ns/draw profiled) and **Diligent D3D12 bindless** (31.2) [measured]

| nvrhi entry point | share | ns/draw | own | runtime | driver | | Diligent entry point | share | ns/draw | own | runtime | driver |
|---|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|
| `drawIndexed` | 43.9 % | 11.0 | 2.7 | 1.1 | 7.2 | | `DrawIndexed` | 46.7 % | 14.6 | 8.6 | 0.7 | 5.2 |
| `RecordAsteroids` own (staging fill) | 28.4 % | 7.1 | | | | | `RenderSubset` own (writes into the mapped buffer) | 43.0 % | 13.4 | | | |
| `writeBuffer` (4.8 MB per frame + `CopyBufferRegion`) | 14.9 % | 3.7 | 3.5 | | 0.2 | | `Flush` | 7.3 % | 2.3 | 0.2 | 0.6 | 1.5 |
| `executeCommandLists` | 5.7 % | 1.4 | | 0.5 | 0.9 | | `UpdateBuffer`, clears, `SetPipelineState`, `SetRenderTargets` | 2.4 % | 0.7 | | | |
| `open`/`close`/clears/`setGraphicsState` | 7.0 % | 1.8 | 0.3 | 0.3 | 1.2 | | | | | | | |

### 2.2 D3D12, 8 threads (main thread and one worker)

[measured]

| | native | Diligent tex_mut | nvrhi tex_mut (no page split) |
|---|---:|---:|---:|
| main thread render + submit, profiled | 0.466 ms | 1.256 ms | 1.629 ms |
| main thread: recording its own 6,250-draw subset | 0.224 ms (48 %) | 0.765 ms (61 %) | 1.197 ms (73 %) |
| main thread: `ExecuteCommandLists` / `executeCommandLists` (incl. driver, kernel submit) | 0.047 ms | 0.112 ms | 0.061 ms |
| main thread: other serial work (`Reset`/`Close` of pre+post lists, `Signal`; Diligent `Flush`, command-list `Release`, `FinishFrame`; nvrhi `open`/`close`/clears of 2 lists) | 0.047 ms | 0.067 ms | 0.071 ms |
| main thread: spinning for workers (`SwitchToThread`, sampled) | 0.034 ms | 0.189 ms | 0.140 ms |
| main thread: off-CPU (yielded or pre-empted, not sampled) | 0.114 ms | 0.124 ms | 0.159 ms |
| one worker: recording its subset | 0.229 ms = 36.6 ns/draw | 0.774 ms = 124 ns/draw | 1.198 ms = 192 ns/draw |
| worker split (layer own / runtime / driver / app) | - / 3.6 / 30.6 / 1.7 | 77.1 / 6.8 / 34.6 / 4.7 | 164.7 / 2.2 / 20.9 / 3.3 |
| worker entry points | `SetGraphicsRootConstantBufferView` 14.8, `DrawIndexedInstanced` 14.7, `Reset`+`Close` 3.5 | `DrawIndexed` 87.3 (own 48.3, driver 32.8), `MapBuffer` 26.7, `Begin` 2.5 | `setGraphicsState` 135.5 (own 126.0, driver 8.4), `writeBuffer` 30.3, `drawIndexed` 22.0 |

### 2.3 Vulkan, 1 thread

**Diligent Vulkan tex_mut** (101.5 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | driver |
|---|---:|---:|---:|---:|
| `DeviceContextVkImpl::DrawIndexed` (`vkCmdBindDescriptorSets` with dynamic offsets + `vkCmdDrawIndexed`) | 67.7 % | 68.7 | 22.2 | 46.5 |
| `MapBuffer` (DISCARD per draw, `VulkanDynamicHeap::Allocate`) | 16.6 % | 16.8 | 16.8 | |
| `CommitShaderResources` | 5.9 % | 6.0 | 6.0 | |
| `RenderSubset` own | 5.9 % | 6.0 | | |
| `Flush`, `UnmapBuffer`, `SetPipelineState`, `SetRenderTargets`, `Draw` | 3.9 % | 3.8 | 1.9 | 2.0 |

Top layer functions (inclusive / exclusive): `DrawIndexed` 68.7 / 10.1, `MapBuffer` 16.8 / 10.0,
`ShaderResourceCacheVk::WriteDynamicBufferOffsets` 7.5 / 7.5, `CommitShaderResources` 6.0 / 6.0,
`VulkanDynamicHeap::Allocate` 5.6 / 5.2, `PipelineLayoutVk::GetPushConstantInfo` 2.7, `UnmapBuffer` 1.1.

**nvrhi Vulkan tex_mut** (271.5 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | driver |
|---|---:|---:|---:|---:|
| `CommandList::setGraphicsState` | 54.5 % | 148.0 | 123.6 | 24.4 |
| `CommandList::drawIndexed` (`vkCmdDrawIndexed`; on 10 % of draws also `bindBindingSets`) | 28.6 % | 77.8 | 6.6 | 71.2 |
| `CommandList::writeBuffer` -> `writeVolatileBuffer` | 11.6 % | 31.6 | 31.6 | |
| `RecordAsteroids` own | 3.2 % | 8.8 | | |
| `Device::executeCommandLists` (incl. `submitVolatileBuffers` 2.6) | 1.5 % | 4.2 | 2.9 | 1.3 |
| `open`/`close`/clears | 0.4 % | 1.1 | 0.3 | 0.8 |

Top layer functions (inclusive / exclusive): `setGraphicsState` 148.0 / 64.8, `drawIndexed` 77.8 / 4.6,
`bindBindingSets` 52.2 / 22.7, `writeBuffer` 31.6 / 3.8, `writeVolatileBuffer` 23.7 / 13.9,
`ViewportState::ViewportState` 18.4 / 15.2, `insertGraphicsResourceBarriers` 9.2 / 1.6,
`insertResourceBarriersForBindingSets` 7.6 / 3.5, `executeCommandLists` 4.2 / 2.8, `setResourceStatesForBindingSet`
3.1 / 1.8. Hot lines: `vulkan-graphics.cpp:660` (`m_CurrentMeshletState = MeshletState()`, 20.7 + 2.6 memset),
`:658` (`m_CurrentGraphicsState = state`, 15.3), `:659` (`m_CurrentComputeState = ComputeState()`, 15.1), `:661`
(`m_CurrentRayTracingState = rt::State()`, 6.9), `ViewportState::ViewportState` (16 `Viewport` + 16 `Rect`
constructors of a temporary, 18.4 in total), `vulkan-buffer.cpp:273` (`m_VolatileBufferStates[buffer]`, 7.5),
`vulkan-buffer.cpp:365` (the `lock cmpxchg` of `compare_exchange_weak`, **6.5 ns/draw = 2.4 %**),
`vulkan-resource-bindings.cpp:984` (`m_VolatileBufferStates.find`, 4.0), `donut::...::AddRef` (3.5).

**nvrhi Vulkan tex_mut_pc** (158.1 ns/draw profiled) [measured]

| entry point | share | ns/draw | layer own | driver |
|---|---:|---:|---:|---:|
| `setGraphicsState` | 70.3 % | 111.0 | 94.4 | 16.7 |
| `drawIndexed` | 20.6 % | 32.6 | 2.3 | 30.3 |
| `setPushConstants` (`vkCmdPushConstants`) | 5.0 % | 7.9 | 0.7 | 7.2 |
| `RecordAsteroids` own | 2.5 % | 3.9 | | |
| rest | 1.6 % | 2.7 | | |

Hot lines: `vulkan-graphics.cpp:659` 12.0, `:658` 11.2, `:660` 10.2 (+1.5), `:661` 6.6, `ViewportState::ViewportState`
15.1 in total, folded `getDesc` 3.9, `bindBindingSets` `:984` 1.6 / `:941` 1.3.

**nvrhi Vulkan bindless** (40.6 ns/draw profiled): `drawIndexed` 24.6 (driver 23.5), `RecordAsteroids` own 9.6,
`writeBuffer` 4.0, `executeCommandLists` 1.0, rest 1.4. **Diligent Vulkan bindless** (45.7): `DrawIndexed` 30.9 (own
10.0, driver 20.9), `RenderSubset` own 12.3, `Flush` 1.3, rest 1.2 [measured].

## 3. What the hot functions do per draw (source), and what native does not

### 3.1 nvrhi D3D12 (`Donut/nvrhi/src/d3d12`)

Per `tex_mut` draw the application calls `writeBuffer` (volatile CB), `setGraphicsState` (90 % of draws) and
`drawIndexed`. The D3D12 calls that come out are the same as Diligent's and native's: one
`SetGraphicsRootConstantBufferView`, one `SetGraphicsRootDescriptorTable` when the texture changes (native: none),
one `DrawIndexedInstanced`. The layer's own 94 ns/draw (scaled) is bookkeeping around them [measured shares,
source-tied]:

1. `CommandList::setGraphicsState` (`d3d12-graphics.cpp:295-513`), 19.9 ns/draw exclusive: compares every field of
   the new `GraphicsState` with the cached one (`arraysAreDifferent` over 16-element viewport/scissor
   `static_vector`s, blend colour, stencil, index/vertex buffers), calls `commitDescriptorHeaps` (2.4),
   `setGraphicsBindings`, `commitBarriers` (2.2, returns at once: nothing to commit, the resources are permanent), and
   at the end copies the whole `GraphicsState` (`m_CurrentGraphicsState = state`, line 510: about 1 KB of
   `static_vector` payload, 9.4 ns/draw as `vcruntime140` moves).
2. `CommandList::setGraphicsBindings` (`d3d12-resource-bindings.cpp:1078-1186`), 32.0 exclusive / 64.9 inclusive:
   for each of the 2 binding sets a virtual `getDesc()` (folded, 2-3 ns), for the volatile constant buffer an
   `unordered_map` lookup `m_VolatileConstantBufferAddresses[buffer]` (line 1118, 3.6), the root CBV call, a
   `push_back` into a 776-byte `static_vector<VolatileConstantBufferBinding, 32>` on the stack whose constructor
   memsets it first (lines 1086/1135: 4.4 + 12.2), `setResourceStatesForBindingSet` (2.1, loops over an empty
   `bindingsThatNeedTransitions`), and finally the member copy `m_CurrentGraphicsVolatileCBs = newVolatileCBs`
   (line 1185, 776 bytes, 8.7). Native keeps no such state: it writes the GPU address straight into the root CBV call.
3. `CommandList::writeBuffer` (`d3d12-buffer.cpp:547-588`), 25.3 inclusive: `UploadManager::suballocateBuffer`
   (6.7; aligns to 256 bytes, advances a chunk pointer), `memcpy` of 160 bytes into write-combined upload memory
   (5.0), `m_VolatileConstantBufferAddresses[buffer] = gpuVA` (another map lookup, 6.4) and flags. Diligent's
   `MapBuffer` + `UnmapBuffer` cost the same, 22.7, for the same job; native writes the two matrices directly into a
   persistent per-frame upload array (1.2 ns/draw for the whole loop).
4. `CommandList::drawIndexed` -> `updateGraphicsVolatileBuffers` (`d3d12-graphics.cpp:527-546`), 6.7 inclusive: a
   third map lookup per draw (line 535, 4.6) to find out whether the volatile CB was rewritten since the last bind,
   and on the 10 % of draws without `setGraphicsState` the root CBV call.
5. `commitDescriptorHeaps`, `commitBarriers`, `setResourceStatesForBindingSet` and the folded `getDesc` calls: 4-5
   ns/draw of checks that find nothing to do because the renderer uses permanent resource states.

Summary for nvrhi D3D12: three hash-map lookups (15 ns), two large struct copies/memsets per state change (about 35
ns at line 510/1086/1135/1185 together), the allocator and memcpy of the volatile write (12 ns), and state
comparison/validation (about 25 ns) [measured]; the API and driver work underneath is the same as Diligent's
(16.8 vs 18.5 ns/draw scaled) [measured].

### 3.2 The page-split store (`docs/LIMITATIONS.md` 5.1)

With the page boundary forced inside `m_CurrentGraphicsVolatileCBs`, the samples do not pile up on the copy at line
1185 (+8 ns/draw) but on the next store burst, the `GraphicsState` copy at `d3d12-graphics.cpp:510` (+74 ns/draw)
[measured]. `m_CurrentGraphicsState` lies several hundred bytes before the split member in the object
(`d3d12-backend.h:1161` against `:1189`), so no store of line 510 itself straddles a page. The reading consistent with
both this profile and 5.1 is store-buffer back-pressure: the page-split store at 1185 (issued right after the
write-combined memcpy of `writeBuffer`) drains slowly, the following 1 KB of 32-byte stores at line 510 cannot
allocate store-buffer entries, and the sampled instruction pointer stalls there [inferred]. The in-process sampler of
the earlier investigation attributed the stall to the split store itself; sampling skid differs between the two
samplers, and neither attribution changes the cause or the size (+88 ns/draw here, 80 ns there) [measured size,
inferred mechanism].

### 3.3 Diligent D3D12 (`DiligentCore/Graphics/GraphicsEngineD3D12`)

1. `DeviceContextD3D12Impl::DrawIndexed` -> `PrepareForDraw` -> `CommitRootTablesAndViews<0>`
   (`DeviceContextD3D12Impl.cpp:373-453`): when the SRB changed (90 % of draws) `CommitRootTables`
   (`PipelineResourceSignatureD3D12Impl.cpp:617-719`): walks both descriptor heap types for dynamic descriptors
   (none in `tex_mut`: the texture is a MUTABLE variable already in the GPU heap), checks `SetDescriptorHeaps`
   (unchanged), issues one `SetGraphicsRootDescriptorTable` per root table; every draw `CommitRootViews`
   (`:499-577`): one `SetGraphicsRootConstantBufferView` with the dynamic buffer's current allocation. 34.8 ns/draw
   own; the hottest line is the heap check at `:678` (15.7) after the SRB-dependent loads, i.e. per-SRB data that
   changes on 90 % of draws [measured location; which load stalls is not resolved].
2. `MapBuffer` with DISCARD (`DeviceContextD3D12Impl.cpp:1734-1802`): `AllocateDynamicSpace` ->
   `D3D12DynamicHeap::Allocate` (ring allocation, `:1784`, 9.1 + 3.6), `m_MappedBuffers` bookkeeping; the matrix
   stores into the returned write-combined pointer are the application's (`asteroids_DE.cpp:1256-1259`, 2.9 ns/draw).
3. `CommitShaderResources` is cheap (0.4): it only records the SRB (`RootInfo.Set`), the work is deferred to the draw.
4. `Flush` after the subsets (`asteroids_DE.cpp:1503-1508`): 2.7 ns/draw = 0.14 ms per frame of
   `CloseAndExecuteCommandContexts` + `CommandQueueD3D12Impl::Submit`.

### 3.4 nvrhi Vulkan (`Donut/nvrhi/src/vulkan`)

1. `CommandList::setGraphicsState` (`vulkan-graphics.cpp:533-663`), 64.8 exclusive / 148.0 inclusive: after the
   checks and `bindBindingSets`, lines 658-661 overwrite **four** cached state structs:
   `m_CurrentGraphicsState = state` (copy of about 1 KB) and `m_CurrentComputeState = ComputeState()`,
   `m_CurrentMeshletState = MeshletState()`, `m_CurrentRayTracingState = rt::State()`. `MeshletState` and
   `GraphicsState` contain a `ViewportState` = `static_vector<Viewport,16>` + `static_vector<Rect,16>`, whose
   default constructor (`containers.h:50-53`, `std::array` value-initialised) runs 16 `Viewport` and 16 `Rect`
   constructors (`nvrhi.h:106`); the temporaries are then copied over the members. Lines 658-661 plus
   `ViewportState::ViewportState` together are **76 ns/draw** in `tex_mut` and 56 in `tex_mut_pc` [measured]; the
   D3D12 backend only flips `m_Current*StateValid` flags here (`d3d12-graphics.cpp:506-511`) [source].
2. `bindBindingSets` (`vulkan-resource-bindings.cpp:940-1019`), 22.7 exclusive / 52.2 inclusive: builds the
   descriptor-set array and the dynamic-offset array; for the volatile constant buffer `m_VolatileBufferStates.find`
   (line 984, 4.0 + hashing) and a virtual `getDesc()` per set; then one `vkCmdBindDescriptorSets` with 2 sets and
   1 dynamic offset (driver 24.4 under `setGraphicsState`). In `tex_mut` it runs on **every** draw because
   `m_AnyVolatileBufferWrites` is set by each `writeVolatileBuffer` (line 571, or from `updateGraphicsVolatileBuffers`
   inside `drawIndexed` on the 10 % of draws without a state change) [source, measured shares].
3. `writeVolatileBuffer` (`vulkan-buffer.cpp:271-381`), 23.7 inclusive (31.6 with `writeBuffer`):
   `m_VolatileBufferStates[buffer]` map lookup (7.5), three `getQueueLastFinishedID` reads, the version search
   (normally the first candidate), the `compare_exchange_weak` claim of the version (line 365, **6.5 ns/draw**),
   min/max bookkeeping, `memcpy` of 160 bytes into the persistently mapped host-visible buffer, plus
   `referencedResources.push_back(buffer)` in `writeBuffer` (a `RefCountPtr` copy: the `AddRef` 3.5 ns/draw).
4. `drawIndexed` -> `vkCmdDrawIndexed`: 71.2 ns/draw in the driver in `tex_mut` against 30.3 in `tex_mut_pc` and
   23.5 in `bindless` [measured]. The NVIDIA Vulkan driver resolves descriptor-set and dynamic-offset changes at draw
   time, so the per-draw rebinding with a dynamic offset shows up under the draw, not under the bind [inferred].
5. `executeCommandLists` -> `CommandList::executed` -> `submitVolatileBuffers` (`vulkan-buffer.cpp:417-442`): one
   `compare_exchange_strong` per version used by the list, 50,000 per frame at 1 thread = 2.6 ns/draw = 0.13 ms,
   serial on the main thread [measured at 1 thread].

### 3.5 Diligent Vulkan (`DiligentCore/Graphics/GraphicsEngineVulkan`)

`DrawIndexed` -> `PrepareForDraw`: `WriteDynamicBufferOffsets` (7.5) and `vkCmdBindDescriptorSets` with the dynamic
offset of the re-mapped constant buffer, then `vkCmdDrawIndexed` (46.5 ns/draw of driver for bind + draw); `MapBuffer`
with `VulkanDynamicHeap::Allocate` (16.8); `CommitShaderResources` (6.0) records the SRB. One descriptor set, one
dynamic offset per draw, no state-struct resets [source, measured shares].

## 4. Answers

### 4(a) Why nvrhi D3D12 `tex_mut` costs about 38 ns/draw more than Diligent at 1 thread

Scaled to the measured medians: nvrhi 116.8 ns/draw, Diligent 78.6, difference 38.2 [measured medians, profiled
shares]. The difference is entirely the layer's own CPU work; the API runtime and driver cost the same:

| component (scaled ns/draw) | nvrhi D3D12 tex_mut | Diligent D3D12 tex_mut | difference |
|---|---:|---:|---:|
| state/binding work per draw: nvrhi `setGraphicsState` own + `drawIndexed` own; Diligent `DrawIndexed` own (`CommitRootTables`/`CommitRootViews`) + `CommitShaderResources` | 66.8 + 5.7 = 72.5 | 32.5 + 0.4 = 32.9 | **+39.6** |
| per-draw constant data: nvrhi `writeBuffer`; Diligent `MapBuffer` + `UnmapBuffer` | 21.4 | 21.3 | +0.1 |
| D3D12 runtime + driver (same calls: root CBV, root descriptor table on texture change, draw, submit) | 19.9 | 21.4 | -1.5 |
| application loop | 2.9 | 2.7 | +0.2 |
| submit (`executeCommandLists` / `Flush`), open/close/clears, render-target setup | 3.0 | 3.2 | -0.2 |

Where the +40 ns/draw of state work goes (profiled numbers, nvrhi `setGraphicsState` subtree, section 3.1)
[measured]: the `m_CurrentGraphicsVolatileCBs` handling in `setGraphicsBindings` (memset of the stack copy,
`push_back`, member copy at line 1185: 25 ns), the `GraphicsState` copy at line 510 (9 ns), three `unordered_map`
lookups per draw for the volatile CB address (15 ns, one of them inside `drawIndexed`), the field-by-field state
comparison, barrier/heap checks and virtual `getDesc` calls (about 15 ns). Diligent's equivalent state work is the
`CommitRootTables` path (25 ns exclusive) with no per-draw hash lookups and no struct copies.
Conclusion [inferred from the measured attribution]: nvrhi's cost is not in what it asks D3D12 to do, but in the
generality of `setGraphicsState` (full-state diff + full-state copy per call) and in tracking volatile constant
buffers through hash maps and a 776-byte `static_vector` that is rebuilt and copied on every state change. In the
19 % of processes with the page-split placement another 88 ns/draw come on top (section 3.2) [measured].

### 4(b) Why nvrhi Vulkan `tex_mut` costs about 4.7 ms/frame more than nvrhi Vulkan `tex_mut_pc`, and about 7.7 ms more than Diligent Vulkan

Scaled: nvrhi Vulkan `tex_mut` 229.8 ns/draw, `tex_mut_pc` 135.9, Diligent Vulkan `tex_mut` 76.0 [measured medians,
profiled shares]. Profiled, the `tex_mut` - `tex_mut_pc` difference is 113 ns/draw (5.7 ms), measured 94 (4.7 ms);
the components below are profiled ns/draw and their shares:

| component | nvrhi Vk tex_mut | nvrhi Vk tex_mut_pc | difference |
|---|---:|---:|---:|
| `writeBuffer` -> `writeVolatileBuffer` (map lookup, version claim with `lock cmpxchg`, memcpy, `AddRef`) | 31.6 | 0.1 | +31.5 |
| `setGraphicsState` own (state resets 658-661 + `ViewportState` ctor, `bindBindingSets` on every call with dynamic offsets and the `m_VolatileBufferStates.find`) | 123.6 | 94.4 | +29.2 |
| driver under `setGraphicsState` (`vkCmdBindDescriptorSets` with a dynamic offset) | 24.4 | 16.7 | +7.7 |
| driver under `drawIndexed` (`vkCmdDrawIndexed`) | 71.2 | 30.3 | **+40.9** |
| `drawIndexed` own (`updateGraphicsVolatileBuffers` -> `bindBindingSets` on the 10 % of draws without a state change) | 6.6 | 2.3 | +4.3 |
| `setPushConstants` (`vkCmdPushConstants`, driver) | - | 7.9 | -7.9 |
| application loop, submit (`submitVolatileBuffers` 2.6), rest | 14.1 | 6.4 | +7.7 |

So the volatile constant buffer path costs three things [measured]: the write itself (31 ns), a descriptor-set
rebind with a dynamic offset on every draw instead of only on texture changes (nvrhi side +33, driver bind +8), and
41 ns/draw more inside `vkCmdDrawIndexed`. The last is the largest single item; it is driver code without symbols,
attributed to the dynamic-offset rebinding that precedes every draw in `tex_mut` and is absent in `tex_mut_pc`
[inferred]. The `lock cmpxchg` of `writeVolatileBuffer` that an earlier in-process sampler saw at 19-22 % of the main
thread carries **2.4 %** (6.5 ns/draw) here [measured]; the difference between the two samplers is not resolved
(skid of an interrupt-based sampler after a locked instruction, or a different build), but even at 20 % it would not
explain the gap to `tex_mut_pc`.

Against Diligent Vulkan (scaled 229.8 vs 76.0 = +154 ns/draw = 7.7 ms) [measured]: layer own 136.7 + 3.0 (Donut
`AddRef`) vs 35.1 (**+105**), driver 82.7 vs 36.3 (**+46**), application 7.5 vs 4.5 (+3). Both layers map/write a
constant buffer per draw (nvrhi 31.6, Diligent `MapBuffer`+`Unmap` 17.9 profiled) and both bind a descriptor set with a
dynamic offset per draw; the layer-own gap is nvrhi's `setGraphicsState` (123.6 profiled: 76 of it the four
state-struct resets and the `ViewportState` constructors, section 3.4 item 1, which Diligent has no counterpart for)
and `bindBindingSets` (2 sets, hash lookup, virtual calls) against Diligent's `CommitRootTables`-style path (22.2
own under `DrawIndexed` + 6.0 `CommitShaderResources`). The driver gap is `vkCmdDrawIndexed` 71.2 vs Diligent's bind +
draw 46.5 [measured]; nvrhi binds two descriptor sets per draw where Diligent binds one, and nvrhi's dynamic offset
walks a 64 MB, 400,000-version buffer where Diligent's dynamic heap is a ring of pages; which of these the driver
charges for is not resolved [inferred].

### 4(c) What remains in the bindless modes

Here every renderer issues one draw per asteroid and binds once per command list; the per-draw differences are the
instance-data path and the draw wrapper [measured, scaled ns/draw]:

| | native D3D12 | nvrhi D3D12 bindless | Diligent D3D12 bindless | nvrhi Vulkan bindless | Diligent Vulkan bindless |
|---|---:|---:|---:|---:|---:|
| total | 17.8 | 22.7 | 27.4 | 32.8 | 41.6 |
| draw call: driver + runtime under the draw | 5.8 (`DrawIndexedInstanced`) | 7.5 | 5.2 | 19.0 | 18.9 |
| draw wrapper, layer own | - | 2.4 (`drawIndexed` -> `updateGraphicsVolatileBuffers` check, virtual call) | 7.5 (`DrawIndexed` -> `PrepareForDraw`, `GetPrimitiveCount`, flag checks) | 0.9 | 9.1 |
| per-draw constant data (native only: root CBV per draw) | 8.5 | - | - | - | - |
| instance data: application writes | 1.2 (into the persistent upload array) | 6.4 (staging array fill) | 11.8 (`MapHelper` writes straight into write-combined memory) | 7.8 | 11.2 |
| instance data: layer upload | - | 3.3 (`writeBuffer`: 4.8 MB memcpy + `CopyBufferRegion`, `docs/LIMITATIONS.md` 2.1) | 0.1 | 3.2 | 0.1 |
| submit, open/close, clears, state | 2.3 | 3.1 | 2.8 | 1.9 | 2.3 |

Reading [measured, with the per-row mechanism from the source]: nvrhi D3D12 bindless is within 5 ns/draw of native
because nvrhi's `drawIndexed` is a thin wrapper (one virtual call and a flag test, 2.4 ns) and the GPU-side binding
work per draw is gone; what remains above native is the staging + `writeBuffer` copy of the instance data (9.7 against
native's 1.2 for writing the matrices directly into its upload array) minus the root CBV per draw that native still
pays (8.5). Diligent D3D12 bindless pays more in its `DrawIndexed` wrapper (7.5) and in writing instance data directly
into write-combined memory in a tight loop (11.8; nvrhi's cached staging array + one memcpy is cheaper). On Vulkan the
driver's draw costs about 19 ns in both layers (3 x the D3D12 driver draw) and dominates; the layers' own share is
13 % (nvrhi) and 23 % (Diligent).

### 4(d) How native and layer costs split at 8 threads

[measured, D3D12, section 2.2]. The main thread's render + submit column consists of: recording its own 6,250-draw
subset (native 0.22 ms, Diligent 0.77, nvrhi 1.20 - the same per-draw cost as a worker, which is 1.6-2.1 x the
1-thread per-draw cost for every renderer), the serial work, and waiting for the slowest worker:

| main thread, ms per frame | native | Diligent tex_mut | nvrhi tex_mut |
|---|---:|---:|---:|
| own subset recording | 0.224 | 0.765 | 1.197 |
| `ExecuteCommandLists` (incl. driver + kernel submit) | 0.047 | 0.112 | 0.061 |
| other serial: native `Reset`/`Close` of pre+post lists + fence `Signal`; Diligent `Flush`, command-list `Release`, `FinishFrame`; nvrhi `open`/`close`/clears of the main + skybox lists | 0.047 | 0.067 | 0.071 |
| waiting for workers: spinning (sampled) + off-CPU (yielded, not sampled) | 0.034 + 0.114 | 0.189 + 0.124 | 0.140 + 0.159 |
| total (profiled) | 0.466 | 1.256 | 1.629 |

Serial work on the main thread is 0.09-0.18 ms per frame for all three, i.e. 20 % of native's column and 8-14 % of
the layers' [measured]. The layers' 8-thread cost over native (Diligent +0.61 ms, nvrhi +0.99 ms measured) is therefore
the per-draw recording cost on the critical path (the main thread's own subset plus the wait for the slowest worker),
not serial submission; nvrhi's `executeCommandLists` (9 lists) is even cheaper than Diligent's
`ExecuteCommandLists` + `Flush` (0.06 vs 0.14 ms). The wait-for-workers component (0.15-0.32 ms) is the imbalance
between the main thread and the slowest of 7 workers plus dispatch latency; it is of the same order for all three
and is not a layer property [measured size, inferred cause]. On Vulkan `submitVolatileBuffers` adds a serial
`compare_exchange_strong` per volatile version used in the frame (0.13 ms at 1 thread, section 3.4 item 5) to nvrhi's
submit; it was not profiled at 8 threads [measured at 1 thread, extrapolation inferred].

## 5. What did not work or is not resolved

- `wpr`/WPA need elevation; VSDiagnostics gives the same kernel sampled-profile data without context-switch events,
  so off-CPU time at 8 threads is the remainder, not measured.
- The 4 kHz sampler slows the profiled runs unevenly (1-34 %); the shares are robust, the absolute ns/draw are scaled.
- NVIDIA driver code is unsymbolised: "driver under X" is as far as the attribution goes; the Vulkan `vkCmdDrawIndexed`
  cost difference (section 4(b)) and the D3D12 `CommitRootTables:678` stall (3.3) are located but not explained.
- The `lock cmpxchg` share (2.4 % here against 19-22 % reported earlier) and the page-split stall location (line 510
  here against the copy loop at 1185 earlier) differ from the in-process sampler of the previous investigation; the
  sizes agree, the instruction-level attribution of stalls does not.
