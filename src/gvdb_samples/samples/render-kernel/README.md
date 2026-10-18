# Portable custom volume renderer

```text
render-kernel -cuda --input explosion.vbx --output custom.png --width 1024 --height 768
render-kernel -vk --self-test
```

`--step` controls index-space sample spacing (default 0.25), and `--threshold`
controls surface density (default 0.1). `--adapter` and `--validation` select the
device and native debug validation. Artifacts live in
`shaders/render_kernel/<backend>`; the shared remap artifact is in `gvdb_owned`.

The Slang path loads VBX through the public `IGVDBSerializer`/`IGVDBVolume` API,
keeps one immutable `IGVDBSnapshot`, and renders directly from its canonical raw
scalar atlas and raw node/child buffers. The app-only loader explicitly records,
submits, and waits for the upload before returning. The owner and serializer are
then destroyed; the retained snapshot is sufficient for rendering. There is no
second `SlangVolume`, atlas, or topology allocation in the production path.

The custom shader keeps the previous portable equations: sample the first scalar
value at or above the density threshold, estimate the gradient with half-voxel
offsets, shade with diffuse lighting and reflected sky, and use `(0,0,0.1,1)` on
miss. Empty volumes use that same miss color. The camera, CLI options, sample
wavefront, output conversion, and retained CUDA reference files are unchanged.

`--self-test` still checks the original diffuse, oblique reflected-sky, depth,
and miss equations; its fixture now enters through the public serializer.
Optional `gvdb_render_kernel_owned_*` tests compare complete color/depth outputs
against a separately compiled old structured-atlas binding path, including the
real `explosion.vbx`, signed non-dense slots, and release before submission.
The old GPU owner is only used in that test executable.

The production target builds raw custom and remap artifacts with
`BUILD_TESTING=OFF`. The old structured custom artifact is test-only. Graphics
builds support D3D11, D3D12, and Vulkan without discovering the CUDA SDK; enabled
CUDA builds compile the same custom Slang source to PTX.
