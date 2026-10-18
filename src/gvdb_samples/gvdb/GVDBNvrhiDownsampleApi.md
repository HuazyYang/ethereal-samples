# Dense regional downsample

`GVDBNvrhiDownsampleApi.h` provides a queryable device operation independent of GVDB topology or volume ownership. Link installed `gvdb::nvrhi_api`; include `GVDBNvrhiDownsampleApi.cmake` when building this optional API from source. The optional concrete C++ target is `gvdb::nvrhi_downsample`. Runtime consumers need NVRHI and an offline artifact, without a shader compiler or native backend SDK headers.

Create `IGVDBDownsample` with an NVRHI device and the `GVDBDenseDownsample.slang` / `downsampleDenseRegion` artifact for that device. `prepare(request,budget,&ticket)` copies the complete logical source span and allocates resources without recording. `record({device,commands},ticket)` records once into a caller-open graphics/compute command list, replacing compute state. The caller closes, submits and waits. `ticket->finishAfterCallerCompletion(bytes)` then returns immutable dense X-fast Float32 bytes, borrowed until ticket release; `getValues(bytes)` permits repeated access. Factory and ticket retain the device/resources. Completion works after factory release. There is no implicit submit, wait, readiness check, or host reduction fallback. A successful finish consumes completion once; failed finish preserves the output and permits retry.

## Sampling and arithmetic

The dense source is contiguous Float32, X fastest, with positive XYZ dimensions. `sourceBytes` must cover the logical array; trailing bytes are ignored. Input may be unaligned. Source NaN and infinities follow per-sample arithmetic, with classification rather than NaN payload preservation promised. Every parameter component must be finite; the Float32 subtraction `remap.y-remap.x` must be finite and nonzero. Destination extent XYZ must be positive; its W is ignored but validated.

For destination coordinate `i`, the kernel forms only two endpoints in binary32:

```text
low  = i * destinationExtent / (destinationResolution + 1)
high = (i + 1) * destinationExtent / (destinationResolution + 1) - 1
```

Endpoint division by the positive integer `resolution+1` uses a correctly rounded binary32 integer quotient/remainder implementation. It needs at most 20 shift steps and no 64-bit arithmetic, avoiding vendor approximate division that can turn `6/3` into the predecessor of two. The numerator remains the separately rounded Float32 product; no epsilon or integer snapping is applied.

Each endpoint is transformed by the three affine rows with ordered, noncontracted binary32 multiply/add operations. Components truncate toward zero. Low clamps to `[0, sourceResolution-1]`; high clamps to `[low, sourceResolution-1]`. Reflections do not reorder endpoints; this is not an eight-corner transformed bounding box. Inclusive nested Z/Y/X loops visit X fastest. The kernel remaps each sample and accumulates serially in Float32, preserving that operation order:

```text
sum += outputMin + (sample-inputMin)*(outputMax-outputMin)/(inputMax-inputMin)
```

The Vulkan entry explicitly requests `RoundingModeRTE 32` through `SPV_KHR_float_controls`; it requires `shaderRoundingModeRTEFloat32`. This keeps the serial addition recurrence in round-to-nearest-even mode instead of an implementation default that may truncate. For example, the ordered remapped samples `33554432, 3, -33554432, 7` sum to 11 with RTE32 and 7 with round-toward-zero. The Vulkan backend rejects a shader requiring an unsupported rounding width before native shader creation.

The GPU returns sum bits and an exact integer sample count. Completion computes `float(double(sum)/double(count))` on the CPU. This retains the existing portable adapter correction: .25 in 21 of 105 samples must average to exactly `.05f`, avoiding a shader reciprocal rounding upward and incorrectly passing a strict `.05` topology threshold. No source samples or endpoint boxes are processed on the CPU. Endpoint boundary tests distinguish exact integer quotients from genuine predecessor/successor results. Other floating operations follow the stated order and hardware IEEE behavior; per-sample remap division can retain graphics-backend rounding differences. There is no blanket byte-parity claim against legacy undefined or approximate behavior.

## Bounds and budgets

Each destination axis is at most `65535*8`. Logical source bytes and the GPU's 8-byte statistics array each fit `UINT32_MAX` bytes. Products and sample counts are checked before allocation. The conservative coordinate proof separately bounds `(dimension+1)*extent` before division by `FLT_MAX/4`; an absolute affine-term bound, including 16 Float32 epsilons of rounding margin, must stay below 2147483000. This may reject a mathematically safe cancellation-heavy transform. A defensive finite/range shader check precedes every integer cast; a failed check reports an invalid zero count and completion leaves results unpublished.

`maxSampleVisits` is the conservative product **source voxel count * destination voxel count**, not the actual sum of transformed region sizes. This can overestimate work substantially but avoids CPU/GPU boundary rounding disagreements in budget validation. `maxInputBytes` counts only copied logical source bytes. For nonempty output, exactly four NVRHI buffers have a total descriptor size `256 + 4*sourceCount + 16*destinationCount`: padded constants, copied source, sum/count statistics, and readback. `maxAllocatedBytes` counts those descriptor bytes, not native allocation granularity or CPU vectors. `maxBuffers` must permit four. Resource budget failures return `FE_OUT_OF_MEMORY`; unsupported address/dispatch domains return `FE_UNSUPPORTED`; malformed descriptors/unsafe coordinates return `FE_INVALID_ARGS`.

Any zero destination axis produces canonical empty bytes `{nullptr,0}`, preserving the requested destination resolution on the ticket. All metadata, positive source geometry and a nonnull complete source span still validate, but source memory is never dereferenced or copied. Work/input/allocation budgets are zero, no GPU resources are allocated and no dispatch is recorded. Factory/device/queue authentication and one-record/one-finish rules still apply. This explicit empty result extends the current portable helper's destination rejection while matching the retained CPU caller's empty-output behavior.

All raw interface outputs are cleared on preparation/factory failure. Borrowed value outputs are preserved on failure. Tickets are private to the originating factory; another factory on the same device cannot record them. Calls on a factory/ticket follow a single-threaded protocol. Retain the ticket until GPU completion; all commands also request normal NVRHI resource liveness tracking.
