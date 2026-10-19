# Render a retained VBX snapshot

`render-to-file` imports a scalar VBX through the public serializer and renders
its retained raw snapshot on D3D11, D3D12, Vulkan or CUDA. Use `--surface` for the
existing isosurface renderer or `--tricubic` for the retained GVDB density-surface
equations. The default remains volume compositing.

```powershell
render-to-file.exe -dx12 --tricubic --width 128 --height 96 --step 1 --output tricubic.png
render-to-file.exe -cuda --tricubic --input volume.vbx --output tricubic.png
```

Tricubic here means the retained `getTricubic` function: a 3x3x3 neighborhood of
linearly filtered integer atlas samples, combined with tensor weights
`(1-t)^2, 2t(1-t), t^2`, where `t=frac(localPosition)*0.5+0.25`. It is not a
substitution of the ordinary trilinear sampler or a Catmull-Rom interpolator.
The implementation uses explicit buffer reads, independent of texture hardware.

The sample helper `renderOwnedTricubic` supports linear sampling with either zero
border (the retained VBX loader's setting and this application's default) or
atlas-wide clamp (the general GP sampler default). Imported apron and unused
slot bytes are retained. A footprint extending beyond a brick's apron can see
another packed brick, so values near those boundaries remain layout-sensitive.
The helper does not rebuild topology, refresh aprons or create a second atlas.

Primary rays march from hierarchy-biased leaf entry with the requested direct
step. The first sample at or above the isovalue receives a fine-step correction
only when it forms a finite bracket; correction stays within the near clip.
The gradient uses negative differences at +/-0.5 with the same tricubic sampler.
A zero/near-zero gradient uses the opposite ray direction. Shading uses ambient
0.1 and diffuse 0.9; shadows deliberately use the retained trilinear query.
Optional packed RGBA8 color is point sampled; scalar-only volumes use white.
The application's camera still frames input bounds, as in its other portable
modes, rather than using the original hardcoded explosion camera.

`TricubicOptions` exposes fine step (positive, at most 1,000,000), epsilon,
shadow amount/bias, leaf-sample and leaf-visit limits, and addressing mode.
The helper requires ray-distance clipping (`direction00.w=0`). Positive infinity
depth is a miss; negative depth is traversal exhaustion. The application rejects
invalid/exhausted output. Empty snapshots produce background without GPU reads.
Publications and queued resources remain retained by the normal NVRHI contract.

Finite-bracket refinement, near-clip protection, stable degenerate normals and
geometric DDA are intentional repairs to legacy extrapolation/NaN/traversal edge
behavior. Cross-backend tests do not establish exact legacy screenshot parity.
Reference sources remain present while the project's broader acceptance gates
are open. Implementation and verification records are archived in the aggregate
repository's `docs/plans/2026-10-19-gvdb-tricubic-surface.md`.
