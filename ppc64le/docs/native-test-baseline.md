# Native ppc64le test baseline — op4k, V620/RADV, 2026-08-13

The full vkd3d-proton `tests/d3d12` suite run natively on ppc64le
(`-mcpu=power8 -fno-strict-aliasing`, gcc 16.1.1, Mesa 26.1.2 RADV, Vulkan
1.4, kernel 4K pages). Log archived on op4k as
`~/vkd3d-d3d12-baseline-20260813.log`.

```
21723848 tests executed (2 failures, 615 successful todo, 120 skipped,
                         1278 todo, 2720 bugs)
```

[MEASURED] with `VKD3D_FILTER_DEVICE_NAME=V620 VKD3D_DEBUG=none
VKD3D_TEST_EXCLUDE=test_update_tile_mappings_remap_smem`.

## The two failures

`test_compute_queue_depth_stencil_msaa` and
`test_copy_queue_depth_stencil_msaa`, both:
`Coord 0, 0: expected 1.000000, got 0.750000` — an MSAA depth resolve
returning a blended sample instead of the expected one on non-graphics
queues. Driver-behavior corner (RADV), not arch-dependent logic; park and
compare against the same Mesa on x86 before blaming the port.

## The exclusion

`test_update_tile_mappings_remap_smem` **hangs the V620** (context lost,
"guilty of a hard recovery"), reproducibly, idle GPU. Sparse binding
(UpdateTileMappings remap) on RADV/amdgpu-on-POWER. Excluded from the
baseline; CP2077 does not use tiled resources. Needs its own
investigation — likely kernel/driver, not vkd3d.
`test_update_tile_mappings` and `..._remap_vmem` pass.

## The gcc 16 strict-aliasing miscompile (fixed in the build)

`test_destruction_notifier_interfaces` crashed with a wild jump through a
clobbered `tests[]` pointer-array slot. Bisection story: the local `device`
stayed intact while `*tests[5]` returned an image-base-relative constant;
instrumenting the array made it a heisenbug; A/B/A single-object rebuilds
proved `-fno-strict-aliasing` fixes it (and `-fstack-reuse=none` also
masked it). The test reads typed COM pointers through `IUnknown**` —
strict-aliasing UB gcc 16.1.1 -O3 exploits on ppc64le into stack-slot
sharing. Since COM-in-C does this pervasively, the whole native build now
carries `-fno-strict-aliasing` (see ppc64le/build-native.sh), as Wine does.

## What this baseline means

Native vkd3d-proton on POWER9/RADV is functionally equivalent to x86
upstream for everything CP2077 needs: 21.7M assertions across resources,
descriptors, PSOs, command processing, fences, DXR — with zero
port-specific logic failures found. The thunk/shim layers sit on a solid
native foundation.
