# Interactive mixed scene through Slang

`interactive-optix` now uses `SlangSceneRT` and the shared volume viewer. D3D12,
Vulkan RT, and CUDA/OptiX execute the same offline Slang library through NVRHI.
The application has no CUDA, OptiX, or backend SDK calls. The original entry point
remains in `legacy-main.cpp` for the legacy build.

The scene retains Lucy at scale 100, the explosion transform
`world = .25 * (index + {-125,-160,-125}) + {44,0,16}`, the original four-segment
16,384-entry transfer table with inclusive lookup endpoint 16,300, and both original
material records. Camera orbit starts at {-20,30}, distance 400; light orbit {45,45},
distance 200. Horizontal FOV 30, near .1/far 5000, background {.1,.2,.4}, volume
steps {.5,16,.5}, extinction {-.25,1,0}, range {.1,0,.3}, cutoff {.001,.001,0},
and epsilon .001 match the retained scene.

```powershell
interactive-optix -dx12
interactive-optix -vk --frames 4 --width 640 --height 480 --screenshot preview.png
interactive-optix -cuda --headless --samples 64 --screenshot render.png
interactive-optix -dx12 --headless --shading levelset --samples 2 --verify
interactive-optix -cuda --self-test
```

`--adapter N` selects the backend adapter. `--validation` enables native graphics
validation. CUDA runs headless; D3D12 and Vulkan provide interactive windows and
headless captures. `--mesh`/`--volume` override the input assets. `--translate X Y Z`
sets the volume translation. D3D11 does not provide the required ray-tracing pipeline.

The Inspector retains shading selection, volume translation, and max-sample control.
Left drag orbits, middle drag pans, right drag changes distance, and Shift selects
the light orbit/distance. Wheel adjusts camera distance. Keys 1..4 select deep,
trilinear, empty-skip, and level-set modes; R resets accumulation; [ and ] adjust
sample limit. X/Y/Z translate along positive axes, A/S/D along negative axes.
Camera, light, mode, translation, and resolution changes reset accumulation.
At the sample limit the dataset RNG frame advances and sampling restarts.
`--samples N` requests a bounded capture; `--screenshot` alone captures 64 samples.
`--frames N` bounds a window or headless run by rendered sample count.

`--self-test` uses a two-brick analytic volume and triangle to check all modes,
attenuation, original scene defaults, transforms, controls, and sample rollover.
`--verify` traces mesh-only and volume-only copies to ensure both actual assets
contribute geometry. These checks do not establish image parity with legacy OptiX;
that requires separately captured and compared images.

Validation on the RTX 4050 completed on 2026-10-18: all 15 CTests passed
(three analytic cases and all four asset modes on three APIs). D3D12 and Vulkan
native validation passed for the analytic cases and bounded Inspector windows.
The 160 x 120, three-sample window captures exactly matched their corresponding
headless images. CUDA Compute Sanitizer reported zero errors for the analytic
suite and all four real-asset modes. At 128 x 96, isolated Lucy covered 713 pixels;
the combined image included 1,107 visible-volume pixels in deep/empty/level-set
modes and 708 in surface mode. These counts use sample-zero coverage probes.

Fresh legacy comparisons at 1024x768 and eight samples are recorded in
`build/interactive-checkpoint-4-comparison.json`. Mean absolute RGB difference
on the 0-255 scale is 1.183 for deep volume and 2.028 for trilinear surface;
4.54% and 6.99% of pixels respectively differ by more than eight channel levels.
Maximum channel differences are 250 and 231. Independent stochastic samples,
corrected accumulation and pixel-center rays preclude a bitwise parity claim.
Captures are `build/interactive-{legacy,new}-{volume,surface}.png`.
