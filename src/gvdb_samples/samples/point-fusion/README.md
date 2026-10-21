# Slang point-fusion example

The original 3,028-box procedural city is scanned on the GPU and fused into a
persistent sparse level set through the public `IGVDBVolume`, `IGVDBPointOps`,
`IGVDBGather`, and `IGVDBApronOps` interfaces. D3D11, D3D12,
Vulkan and CUDA share the same Slang scanner, compaction, atlas remap and gather
kernels. CUDA provides the headless path; graphics APIs also provide a window.

```text
point-fusion -dx12 --color
point-fusion -vk --orbit --objects --points
point-fusion -cuda --color --speed 1 --frames 2 --output city.png
point-fusion -dx11 --self-test
point-fusion -dx12 --color --frames 2 --screenshot window-city.png
```

The default 240x180 scanner retains the 90-degree horizontal field of view,
initial position `(1222,20,1222)`, yaw 180/pitch zero, four-block range and original
city scale. The display camera uses 65 degrees. Each displayed frame moves the
scanner by `speed` index voxels, then scans/fuses once while scanning is enabled.
Headless `--frames N` executes that same update N times before rendering.
On graphics APIs, `--frames N --screenshot image.png` captures the final window
frame and closes it. `--headless` explicitly selects offscreen execution; CUDA
always runs headlessly. `--output` is an alias for the screenshot path.

Key 1 toggles scan points, 2 topology and Space scanning. Stopping the scanner
also sets speed to zero. F toggles FPV/orbit, O city boxes, C color (clearing the
volume), and R clears the accumulated volume. The original Inspector toggles
have keyboard equivalents. In FPV or with Shift held, left dragging changes
scanner angles, middle dragging changes its height in `[0,500]` and stops it,
and right dragging changes speed. In orbit mode, those buttons orbit, pan and
change distance. The wheel adjusts speed in FPV and distance in orbit.

The public owner manages a persistent union of 32-voxel support leaves, matching the
legacy `fromBranching(3,3,3,3,5)` argument order (root to leaf). Each leaf
contains 512 subcells of width 4. Subcell lists are built in
stable scan order on the CPU, while scalar/color fusion runs in Slang. A GPU
remap preserves prior scalar and raw RGBA values when the root or atlas layout
changes. New voxels start at distance 3 and transparent black. All channels
share the same topology. These host steps are deliberate migration choices;
the app stages compact scanner outputs explicitly, converts them to a retained
public PointSet, then records topology accumulation, gather, and apron updates.
It has no second SlangVolume, PackedVolume, or SlangAtlas owner. Immutable
snapshots supply both the raw scalar/color buffers and rendering metadata.
Point overlays borrow the retained PointSet; topology overlays use the public
host graph and refresh on topology revision changes.

Preparation/publication is atomic per operation, not across the whole scan. If
a later operation fails after topology publication, the last completed display
snapshot remains retained and scanning requires reset. Ordered submissions use
the same graphics queue. The explicit point conversion/readback staging phase
waits before CPU topology work; rendering adds no hidden owner queue.

The scanner fixes the original shared-seed race with one LCG state per pixel.
It explicitly marks misses and compacts valid hits, so a real point at the
origin is preserved and outside-city scans cannot reuse stale points. Thus the
new scan sequence is deterministic but does not claim pixel equivalence to the
racy legacy CUDA sequence. The city bounds and colors were compared against
verbatim original generation code: all 3,028 boxes match on the current host.

The renderer remains in level-set Surface mode; generic V/I viewer shortcuts are
consumed because density modes do not share the level-set background contract.
The surface renderer treats missing sparse space as distance 3 and samples the
raw RGBA channel with the original point filtering. The viewer uses
its portable lighting rather than the legacy CUDA Phong/shadow implementation;
point overlays are small screen-space glyphs rather than voxel-length ticks.
The empty-volume display uses an actual empty published snapshot. The raw surface
kernel emits the requested background and positive-infinite depth before any
topology access; no dummy leaf or fabricated scan point is inserted.

`--self-test` compares jittered scan hits, packed colors, compact pixel order and
LCG state against independent CPU slabs. Persistent fusion tests cover repeated
scans, negative coordinates, root/atlas relocation, scalar/color aprons, invalid
and empty scans, and channel reset. They also check fractional support coverage
across positive/negative leaf boundaries, monotonic channel revisions, and
retained old buffer bytes after later publication and owner replacement. Input fixtures cover FPV/orbit/Shift
precedence and camera/speed clamps. CTest also executes two full-resolution city
scans on every enabled API and requires finite rendered surface hits. See
`docs/point-fusion-slang-plan.md` for the complete migration and test contract.

The remap, point conversion, gather, apron mutation, and raw surface artifacts
are production dependencies and are generated with `BUILD_TESTING=OFF`,
including graphics-only builds without CUDA SDK discovery.

The portable Slang/NVRHI application is the supported build.
`PointFusionUpdate.slang` remains as the prior portable gather reference; the
application now dispatches the shared public gather implementation.
