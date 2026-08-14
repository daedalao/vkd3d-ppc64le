# D3D12 boundary analysis — native ppc64le vkd3d-proton behind an x86-64 PE shim

The D3D12 analog of dxvk-ppc64le's `docs/dxvk-boundary-analysis.md`. Read that
project's `docs/fex-thunk-architecture.md` and `docs/thunk-runtime.md` first;
this document only records what D3D12/vkd3d-proton *changes* relative to the
proven D3D11 design, with evidence.

Evidence marks: **[MEASURED]** a command was run and output observed ·
**[CODE]** read from source at file:line · **[SPEC]** inference, with the
confirming test named.

Baseline facts this rests on (established 2026-08-13):

- vkd3d-proton master (`731c4aae` + ppc64le scaffolding) **builds natively on
  ppc64le out of the box** — 282/282 targets, `-mcpu=power8`, zero source
  changes, only printf-format warnings. [MEASURED, op4k, gcc 16.1.1]
- The native Linux build produces `libvkd3d-proton-d3d12.so` (8 flat exports,
  a loader shim) and `libvkd3d-proton-d3d12core.so` (the implementation)
  [CODE] `libs/d3d12/meson.build`, `libs/d3d12core/meson.build`.
- All SSE is behind `#ifdef __SSE2__` with memcpy/no-op fallbacks [CODE]
  `include/private/copy_utils.h:22-28,103-113`, `vkd3d_spinlock.h:27-36`;
  atomics are GCC `__atomic` builtins [CODE] `vkd3d_atomic.h:197+`; rdtsc
  falls back to `clock_gettime` [CODE] `vkd3d_common.h:336-348`.

## 1. The boundary, restated for D3D12

Same shape as D3D11: the game's x86-64 PE code holds COM proxies whose vtable
slots cross into native ppc64le vkd3d-proton; the Vulkan thunk underneath is
no longer crossed at all. The boundary stays **three functions**
(`call`, `call_float`, `call_entry`) for the same reason (FEX's host
trampoline allocator never frees).

What d3d12.dll exports (the flat entries) [CODE] `libs/d3d12/main.c`:

```
D3D12CreateDevice                          adapter IN-iface, riid void** out
D3D12GetDebugInterface                     riid void** out
D3D12GetInterface                          rclsid+riid void** out
D3D12CreateRootSignatureDeserializer       riid void** out
D3D12CreateVersionedRootSignatureDeserializer  riid void** out
D3D12SerializeRootSignature                ID3DBlob** out ×2
D3D12SerializeVersionedRootSignature       ID3DBlob** out ×2
D3D12EnableExperimentalFeatures            riid-less; IIDs array IN
```

plus `D3D12SDKVersion`/`D3D12SDKPath` data exports for AgilitySDK-aware
callers (present as data in the PE shim; not calls).

**The adapter parameter is ignored by the native build** [CODE]
`libs/d3d12core/main.c:684-688` (`FIXME("Ignoring adapter.")`) — device
selection is by vkd3d's own enumeration, steered by `VKD3D_FILTER_DEVICE_NAME`
/ `VKD3D_VULKAN_DEVICE`. Shim v1 therefore passes `adapter = NULL` and does
not attempt to translate the guest's `IDXGIAdapter`. Single-GPU-of-interest
(V620) on the deployment box makes this correct in practice; revisit when the
native-DXGI interop lands (§6). The same line carries upstream's own TODO:
"We need to attempt to dlopen() native DXVK DXGI" — upstream anticipates
exactly the arrangement this project builds.

## 2. Where D3D12 breaks the D3D11 generator's invariants — and the resolutions

dxvk-ppc64le's `check_invariants()` **fails generation** on: aggregate
returns, by-value aggregates other than 8-byte {int32,int32}. D3D12 has both.

### 2.1 Aggregate returns exist (and are already solved by widl's C form)

`GetCPUDescriptorHandleForHeapStart`, `GetGPUDescriptorHandleForHeapStart`,
`GetResourceAllocationInfo(+1,2)`, `GetDesc(+1)` (resource/heap flavors),
`GetAdapterLuid`, `GetLUID`, `GetCustomHeapProperties`, ... — the census
tooling enumerates them mechanically (`aggregate_return` flag in
`interfaces.json`, cross-checked two independent ways).

The resolution requires no new transport. In the widl-generated **C vtable**
— which is what native vkd3d-proton compiles and implements — these slots
have an explicit return-slot pointer [MEASURED, `ppc64le/idl/gen/vkd3d_d3d12.h:18982`]:

```c
D3D12_RESOURCE_ALLOCATION_INFO * (STDMETHODCALLTYPE *GetResourceAllocationInfo)(
    ID3D12Device *This, D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT ..., ...);
```

And MSVC's x64 C++ ABI returns **every** class/struct/union from a member
function via hidden pointer in the same position (immediately after `this`),
regardless of size — this is the well-known D3D12 descriptor-handle ABI fact
that mingw builds had to work around. So an MSVC-compiled game's call site
and the native ELF slot agree: `(this, retptr, args...)`, return value =
retptr. **These slots ride the generic uint64 transport unchanged**; the
generator marks them only so the ms_abi forwarder declares the right arity
and pointer return.

[SPEC] risk: a PE game built with mingw-gcc would return 8-byte handles in
RAX instead. Mingw-built D3D12 games are essentially nonexistent (the
Windows SDK headers are MSVC-shaped), and Wine's own d3d12 consumers use the
MSVC convention for exactly this reason. Accepted.

### 2.2 By-value aggregates: two descriptor handles, both 8 bytes

`D3D12_CPU_DESCRIPTOR_HANDLE {SIZE_T}` and `D3D12_GPU_DESCRIPTOR_HANDLE
{UINT64}` are passed by value throughout the command-list surface
(`ClearRenderTargetView`, `SetGraphicsRootDescriptorTable`, ...). 8-byte POD:
MS-x64 passes any 1/2/4/8-byte aggregate in one GPR; SysV classifies one
INTEGER eightbyte; ELFv2 passes small aggregates in GPRs. `uint64_t`
transport is correct for all three. Same argument as dxvk's LUID/SIZE census,
extended to 8-byte one-member structs. The census must verify **no by-value
parameter larger than 8 bytes exists** anywhere in the surface, and fail
generation otherwise.

### 2.3 Float-class slots

D3D11 had 5 prototypes / 21 slots. D3D12's by-value float methods (to be
confirmed mechanically by the census — the list below is from reading the
header, and generation must fail if the census finds one not listed):

- `ClearDepthStencilView(..., FLOAT Depth, UINT8 Stencil, ...)` — one float
  arg mid-signature, plus a **by-value descriptor handle** first.
- `OMSetDepthBounds(FLOAT Min, FLOAT Max)` (GraphicsCommandList1+).
- `ID3D12Device::SetStablePowerState`? no — takes BOOL. 
- `RSSetShadingRate`? enums only.
- AtomicCopyBuffer family — UINT64 only.

Everything else takes `const FLOAT[4]`/`const FLOAT*` (pointers — legal on
the integer path). Expect ~2 prototypes across the command-list versions;
same shape-id design as dxvk (`call_float` with shape).

### 2.4 The census consequences

The generator inherits dxvk's refuse-to-guess posture: `ARRAY_SPECS` for
every interface array (e.g. `ExecuteCommandLists(UINT, ID3D12CommandList*
const*)`, `OMSetRenderTargets`'s descriptor arrays are **descriptor handles,
not interfaces** — no proxying!), riid-driven `void**` outs resolved before
crossing, raw `void**` (memory) enumerated by name:

- `ID3D12Resource::Map(UINT, const D3D12_RANGE*, void **ppData)` — mapped
  memory, passthrough (shared address space; §5 TSO note).
- `ID3D12Heap::GetDesc`-style — aggregate returns, §2.1.
- `ID3D12StateObjectProperties::GetShaderIdentifier` — returns `void*` into
  host memory; guest memcpys 32 bytes; passthrough correct under shared
  address space.
- `ID3D12PipelineLibrary::Serialize(void*, SIZE_T)` — guest buffer the host
  writes; shared address space.

## 3. Objects and lifetime

Same proxy/interning design as dxvk (key = (host ptr, iface id), 64 shards,
1→0 under the shard lock, wrap consumes one host reference). D3D12-specific
notes:

- **ID3D12 objects are created in bursts** (CP2077: thousands of PSOs,
  resources). The interning table is the same structure that survived 160k
  contended crossings in dxvk's stress test; no change.
- **`GetDevice(riid, void**)`** on every `ID3D12DeviceChild` — riid-driven
  out; must intern back to the existing device proxy (dxvk proved the
  identical pattern with `GetParent`).
- **`SetPrivateDataInterface`** — unwrap IN `IUnknown*`; the host then holds
  a guest-side... no: it holds a **host-side reference to whatever was
  passed**. A game passing its *own* IUnknown implementation here would hand
  vkd3d a guest vtable. dxvk ships the same hole for D3D11; games use
  SetPrivateData (names) not interfaces in practice. STRICT-flagged, not
  solved. [SPEC]
- **`GetPrivateData` with an interface GUID** — same unfixable-by-signature
  hole as dxvk §8.3.1. Recorded.

### Guest-implemented interfaces: the D3D12 inventory

The question dxvk's doc said decides everything (§6.4): does the API force
vkd3d to call guest code outside a guest→host call?

- **ID3D12Fence + SetEventOnCompletion** — not an interface callback but an
  event HANDLE signaled from vkd3d's internal fence worker threads
  (host threads!). This is the real async hazard, and it has a clean
  resolution — §4.
- **ID3D12InfoQueue1::RegisterMessageCallback** (sdklayers) — a genuine
  function-pointer callback fired from arbitrary host threads. Debug-layer
  only; CP2077 retail never touches it. Policy: `callback_stub` semantics —
  accept-and-warn or E_NOTIMPL; never call.
- **D3D12EnableExperimentalFeatures / device factory** — data only.
- **Everything else vkd3d-proton consumes is data or its own objects.**
  Unlike D3D11 (no user callbacks there either), D3D12 core has **no
  app-implemented COM interfaces at all** in the paths a game exercises.
  [SPEC — confirmed by the census: no method parameter is an interface type
  the app could implement other than IUnknown via SetPrivateDataInterface.]

## 4. Fences and events — the design

**Native fact:** on the Linux build, the `HANDLE` given to
`SetEventOnCompletion` is treated as an **eventfd file descriptor** and
signaled with `write()` from vkd3d's worker threads [CODE]
`include/private/vkd3d_native_sync_handle.h:55,91-105` (`fd =
(int)(intptr_t)os_handle`).

**FEX fact:** guest and host share one kernel, one process, one fd table.

**Wine fact (op4k deployment):** the box runs Wine with **ntsync**
[MEASURED: `~/ntsync-mod`, `cp2077-ntsync-wander.perf` on op4k] — guest event
HANDLEs are ntsync objects, not eventfds, so we cannot pass a Wine event's fd
straight into vkd3d even where esync would have allowed it.

**Design: one guest pump thread + host-side completion queue.** All
host→guest transitions remain synchronous returns from guest-initiated
calls, satisfying FEX's `CallCallback`-only-on-FEX-threads rule:

1. Host stub (`libvkd3d-thunk-host`) keeps, per device: a queue of
   `(guest_event_cookie)` and a doorbell eventfd.
2. Shim `ID3D12Fence::SetEventOnCompletion(fence, value, hEvent)`:
   - `hEvent == NULL` is a **blocking wait** by API contract — forward as a
     plain synchronous crossing (guest thread blocks in host; legal).
   - otherwise the guest stub crosses with `(fence, value, cookie)` where
     cookie is the guest HANDLE value; the host stub allocates a pooled
     eventfd E, calls native `SetEventOnCompletion(fence, value, E)`, and a
     single host reaper thread (one per device, owned by the host stub —
     native code, free to block) `poll()`s all pending E's; on completion it
     pushes the cookie and writes the doorbell.
3. The PE shim creates **one guest pump thread** (Win32 `CreateThread`) at
   device creation. It loops: cross into host `wait_next_completion()`
   (blocks on the doorbell — a synchronous guest→host call), returns a
   cookie, calls Win32 `SetEvent((HANDLE)cookie)` in guest code, repeats.
   Wine handle ownership stays entirely guest-side; ntsync vs esync vs
   server events is irrelevant.

Costs: one host thread per device, one guest thread per device, one extra
hop per event-signal (µs against GPU fence latencies). `GetCompletedValue`
(CP2077's hot fence path) is a plain synchronous read crossing — untouched.

Multi-wait fences (`ID3D12Device1::SetEventOnMultipleFenceCompletion`) ride
the same queue. Shared fences/resources (`CreateSharedHandle`) are Windows
NT-handle machinery vkd3d only implements on Win32 [CODE]
`d3d12_shared_fence_*` paths guarded; shim returns E_NOTIMPL — CP2077 does
not use them.

## 5. Memory, descriptors, TSO

- **CPU descriptor handles** are host pointer-sized values; the guest does
  `ptr + i*increment` arithmetic on them and passes them back, never
  dereferences. Shared address space makes this transparent. GPU handles are
  opaque uint64. No translation anywhere.
- **Map** returns a host pointer to VkDeviceMemory; the 64-bit guest writes
  through it directly (dxvk ships the same). The TSO caveat transfers
  verbatim: guest x86-64 is TSO, POWER9 is weakly ordered; whether guest
  writes are visible to the GPU at ExecuteCommandLists depends on
  fastppcx86's TSO emulation — same assumption the working Vulkan thunk +
  DOOM already rest on. [SPEC]
- **WriteToSubresource/ReadFromSubresource** — guest buffers read/written by
  host during the call; shared address space, synchronous. Fine.

## 6. The swapchain / DXGI seam (cross-agent interface)

Proton's D3D12 games get swapchains from **dxvk's DXGI**, which asks the
D3D12 command queue for `IDXGIVkSwapChainFactory` (this repo's
`include/vkd3d_swapchain_factory.idl`) and drives presentation itself.
In the twin-native world both ends are native ppc64le `.so`s in the same
host process, so the factory handoff is **native↔native** — the thunk layer
never sees it. What must happen for that to work:

1. dxvk-dxgi's *shim* (their agent's pe-shim) receives the guest's
   `ID3D12CommandQueue` proxy in `CreateSwapChainForHwnd`. It must obtain
   the underlying **host** pointer to hand to native dxvk-dxgi. That is a
   cross-runtime unwrap: our thunk runtime exports
   `vkd3d_thunk_unwrap(IUnknown*) -> host ptr` (and a probe symbol so their
   shim can detect us). Their D3D11-side already has the identical need in
   reverse for D3D11On12.
2. Native dxvk-dxgi QIs the host queue for IDXGIVkSwapChainFactory and
   proceeds exactly as on x86 Linux (their WSI/X11-foreign work carries the
   HWND problem, already solved for D3D11).

Until that lands, native validation uses vkd3d-proton's **test suite and
demos** (xcb/X11 swapchain via `enable_extras` demos) — no DXGI involved.

## 7. What the PE shim must contain beyond forwarding

- The 8 flat exports + `D3D12SDKVersion`/`D3D12SDKPath` data exports.
- **No WCHAR shim needed for core D3D12** — the D3D12 API has zero
  WCHAR-carrying structs; `SetName(LPCWSTR)` and
  `ID3D12Object::SetPrivateData(WKPDID_D3DDebugObjectNameW)` carry wide
  strings, but they flow guest→host as **byte buffers with explicit or
  NUL-derived lengths, PE-side 2-byte WCHAR**, while native vkd3d-proton
  builds with `WCHAR = unsigned short` too on the widl path [CODE]
  `include/vkd3d_windows.h:88` (`typedef unsigned short WCHAR`) — native
  Linux vkd3d uses 2-byte WCHAR, unlike DXVK-native's 4-byte wchar_t.
  **Verify with the layout probe; if 2-byte on both sides, `SetName` is
  passthrough and the D3D11 project's biggest shim component vanishes.**
- Fence pump thread (§4).
- AgilitySDK surface (`D3D12GetInterface` with CLSID_D3D12SDKConfiguration
  etc.): vkd3d-proton implements the interfaces; passthrough via generic
  slots. CP2077 does not ship the Agility SDK; low priority.

## 8. Testing ladder (mirrors dxvk-ppc64le's, with its lessons pre-applied)

1. **Native**: `tests/d3d12` on op4k/RADV — establishes vkd3d-on-POWER
   correctness with zero thunk involvement. [in progress]
2. **Loopback mock** (`ppc64le/thunk/tests/`): SysV caller + MS-x64 caller
   from day one (dxvk's A1 lesson), negative control that MUST fail with
   forwarders disabled, threaded interning stress, one exemplar per
   marshalling shape **including an aggregate-return slot and a by-value
   descriptor-handle slot** (the two D3D12-specific shapes).
3. **Loopback against real native vkd3d-proton** via dlopen (headless: no
   swapchain needed for CreateDevice→CreateCommandQueue→CreateFence→
   Signal/Wait→CreateCommittedResource→Map/write/ExecuteCommandLists→
   readback — a real GPU round trip with zero WSI).
4. **Under fastppcx86**: same loopback compiled x86-64, then the msabi
   caller, then the PE hop under Wine.
5. **CP2077** (the stated acceptance bar), after the dxvk-dxgi seam (§6).

## 9. Open hazards, ranked

1. **The dxvk-dxgi native seam** (§6) — required for any real game;
   cross-agent coordination; design agreed nowhere yet.
2. **TSO at Map/ExecuteCommandLists** (§5) — inherited assumption, never
   proven at D3D12 write volumes.
3. **vkd3d worker-thread model under FEX** — vkd3d spawns host pthreads
   (queue submitters, fence workers); they never call guest code (§4
   removes the one path), but they do malloc/lock alongside guest threads.
   dxvk's host side does the same and survives DOOM. [SPEC]
4. **`WCHAR` parity** (§7) — probe will settle it.
5. **Aggregate-return mingw-caller edge** (§2.1) — accepted, documented.
6. **Debug layers under thunk** (sdklayers) — generated but untested;
   CP2077 never loads them.
