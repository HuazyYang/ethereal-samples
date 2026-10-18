# Custom Slang render kernel

The default `render-kernel` sample loads a scalar VBX volume, uses ordinary NVRHI
buffers and bindings, and writes `out_rendkernel.png`. Choose `-dx11`, `-dx12`,
`-vk`, or `-cuda`; all four use the same offline `RenderCustom.slang` source.

```text
render-kernel -cuda --input explosion.vbx --output custom.png --width 1024 --height 768
render-kernel -vk --self-test
```

`--step` controls index-space sample spacing (default 0.25), and `--threshold`
controls the surface density (default 0.1). The camera frames the loaded volume's
index-space bounds. `--adapter` and `--validation` use the shared example device
selection. Shader artifacts are built into `shaders/render_kernel/<backend>`.

The custom appearance is the original CUDA sample's algorithm: align samples to
a ray-distance wavefront, stop at the first trilinear density sample at or above
the threshold, estimate its normal with half-voxel differences, and combine
`0.6 * diffuse` with `(0, 0.3, 0.7) * saturate(reflectedEye.y)`. Misses are dark blue;
PNG alpha remains opaque. This is a custom surface kernel rather than the generic
volume or isosurface renderer. Sparse hierarchy lookup uses the shared portable
GVDB layout; no legacy gp or CUDA runtime initialization is required.

`--self-test` checks an analytic sparse ramp's surface depth, diffuse color,
nonzero reflected-sky contribution and miss color. CTest also renders the real
explosion VBX on each enabled backend and rejects nonfinite or empty images.
The original `legacy-main.cpp` and `RenderCustom.cu` remain available when the
legacy GVDB configuration is selected.
