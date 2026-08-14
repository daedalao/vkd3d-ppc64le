# The D3D12 COM thunk layer

The guest↔host boundary for running an x86-64 PE game's D3D12 calls against
**native ppc64le vkd3d-proton** under FEX.  The D3D12 analog of
dxvk-ppc64le's `thunk/`, ported from that design deliberately: read
`ppc64le/docs/d3d12-boundary-analysis.md` for what D3D12 changes and why, and
that project's `docs/thunk-runtime.md` for the reasoning behind every structure
here (interning key, the 1→0 refcount transition inside the shard lock, the
three-arrays-per-interface calling-convention fix, refuse-to-guess marshalling).

```
ppc64le/thunk/
  gen_interfaces.py                  GENERATED-FROM the widl headers -> census
  interfaces.json                    the census: 76 interfaces, 2343 slots
  gen_thunk.py                       HAND   the generator
  generated/vkd3d_thunk_ids.h        GEN    ids, slot counts, IID table, float
                                            shapes, pump table, fixup table and
                                            declarations, per-slot flags, named
                                            slots for the tests
  generated/vkd3d_thunk_guest.cpp    GEN    2008 worker stubs + 2343 ms_abi
                                            forwarders + 3x76 vtable arrays
  generated/vkd3d_thunk_host.cpp     GEN    the generic host dispatcher
  generated/vkd3d_thunk_counts.sh    GEN    the numbers tests/build.sh checks
  runtime/vkd3d_thunk_abi.h          HAND   the boundary contract (not ours to
                                            change; unmodified by this package)
  runtime/vkd3d_proxy.h/.cpp         HAND   interning, refcounts, QI, arrays,
                                            float stubs, ABI mode, the 8 flat
                                            entry points, interop exports
  runtime/vkd3d_struct_fixups.cpp    HAND   the 10 struct-with-interface fixups
                                            (the ONE file here that sees d3d12
                                            types)
  runtime/vkd3d_thunk_host_rt.cpp    HAND   dlopen, entries, float dispatch,
                                            the fence/event pump
  tests/loopback.cpp                 HAND   both halves in one process
  tests/msabi_caller.cpp             HAND   an MS-x64 caller, and its negative
                                            control
  tests/build.sh                     HAND   builds and checks everything
```

The split rule: if the "pack every argument into `uint64_t[10]`" convention can
express it, the generator emits it; if it cannot, it is hand-written in
`runtime/` and a vtable slot points at the hand-written symbol.

## The numbers [MEASURED, `tests/build.sh`]

```
interfaces           : 76
vtable slots         : 2343
  SysV worker stubs  : 2008
  ms_abi forwarders  : 2343 (x86-64 only)
  hand-written       : 335 (228 IUnknown + 23 float-class + 84 struct fixup)
  float-class shapes : 3 across 23 vtable slots
      VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW  slot 47  x11
      VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS       slot 62  x10
      VKD3D_FSHAPE_RS_SET_DEPTH_BIAS         slot 82  x2
D3D12-specific shapes:
  aggregate-return   : 91 slots (explicit __ret, generic path)
  by-value aggregate : 187 slots (<= 8 bytes, one uint64 slot)
interface-pointer marshalling, generated per slot:
  slots marshalled   : 982
    IN  Iface*       : 584
    OUT Iface**      : 0
    Iface array+count: 107
    riid void** out  : 463
  raw void** (memory): 3 (ID3D12Resource::Map; passed through)
  struct with iface  : 105 slots, 123 (slot, struct) pairs, 10 struct types
  refused            : 3
  struct-iface only  : 1 (no other pointer in the signature)
  no pointers at all : 1022
  reconciliation     : 982 + 335 + 3 + 1 + 1022 = 2343
host slot overrides  : 17 (fence/event pump)
      VKD3D_PUMP_FENCE_EVENT         slot 9   x2
      VKD3D_PUMP_MULTI_FENCE_EVENT   slot 45  x15
guest slot overrides : 84 (struct fixups, 10 shapes)
      VKD3D_FIXUP_RESOURCE_BARRIER                 slot 26  x11
      VKD3D_FIXUP_COPY_TEXTURE_REGION              slot 16  x11
      VKD3D_FIXUP_CREATE_GRAPHICS_PIPELINE_STATE   slot 10  x16
      VKD3D_FIXUP_CREATE_COMPUTE_PIPELINE_STATE    slot 11  x16
      VKD3D_FIXUP_LOAD_GRAPHICS_PIPELINE           slot 9   x2
      VKD3D_FIXUP_LOAD_COMPUTE_PIPELINE            slot 10  x2
      VKD3D_FIXUP_CREATE_PIPELINE_STATE            slot 47  x14
      VKD3D_FIXUP_LOAD_PIPELINE                    slot 13  x1
      VKD3D_FIXUP_BEGIN_RENDER_PASS                slot 68  x7
      VKD3D_FIXUP_BARRIER                          slot 80  x4
struct-with-interface reconciliation:
  slots carrying one  : 105
    fixed here        : 84 (runtime/vkd3d_struct_fixups.cpp)
    left + STRICT     : 21
      ID3D12Device5                CreateStateObject            x11
      ID3D12Device7                AddToStateObject             x9
      ID3D12DeviceRemovedExtendedData GetAutoBreadcrumbsOutput   x1
    unaccounted       : 0
```

`OUT Iface**` is **0** and that is not a bug: D3D12 returns every created object
through a riid-driven `void**`, so all 514 of those slots go through the IID
table instead -- 463 in generated stubs and 51 inside the pipeline-state
fixups, which do the same riid dance by hand.  The one non-riid `void**` in the
surface is `ID3D12Resource::Map`, which is mapped memory.

## Running the tests

```sh
ppc64le/thunk/tests/build.sh          # x86-64 box: everything, ~45 s
PPC_MCPU=power8 ppc64le/thunk/tests/build.sh    # POWER box: no ms_abi steps
```

The script regenerates, builds the guest and host halves at `-O0` **and** `-O2`
(an earlier generator in the D3D11 project lost every stub at `-O2`), asserts
the worker/forwarder/vtable counts survived optimisation, disassembles two
`ms_abi` forwarders to prove the convention is really converted, checks the
export list, runs the loopback and the MS-x64 caller, and **requires** the
negative-control build to fail.  It prints `=== ALL OK` and exits non-zero on
any failure.

Last full run on the x86-64 box: **232 loopback assertions, 40 MS-x64
assertions, 0 failures**, negative control exit 139 with 2 `FAIL` lines.

### Environment

| variable | effect |
|---|---|
| `VKD3D_THUNK_ABI=sysv\|ms` | guest vtable convention; default MS-x64 on x86-64 (the deployment target is a PE game), frozen at the first proxy, printed once to stderr. **Guest ELF callers must set `sysv`.** |
| `VKD3D_THUNK_TRACE=1` | log every wrap/release and every refused IID |
| `VKD3D_THUNK_STRICT=1` | abort at the first call that crosses with a struct carrying interface pointers, and make the host dispatcher warn once per untranslated slot |
| `VKD3D_THUNK_STRUCT_WARN=every\|once\|off` | how loudly those slots complain when STRICT is off (default `every`) |
| `VKD3D_THUNK_D3D12_LIB=<path>` | which native library the host half dlopens (default `libvkd3d-proton-d3d12.so`) |

## What is known-wrong, and how you find out

Nothing below is stubbed silently.

1. **Structs that carry interface pointers: 84 of the 105 slots are fixed, 21
   still cross RAW.**  A `D3D12_RESOURCE_BARRIER` holds an `ID3D12Resource*`,
   and structs pass by pointer with no repacking, so without help native vkd3d
   sees a guest `Proxy*` inside them.  `runtime/vkd3d_struct_fixups.cpp` copies
   the aggregate to scratch, rewrites its interface members and lets the copy
   cross; the wiring is `FIXUP_SLOTS` in `gen_thunk.py`, i.e. the same
   `hand_written()` mechanism the float shapes use, so `k<I>_target` and
   `k<I>_vtbl_sysv` point at the fixup and the generic `ms_abi` forwarder calls
   through `target[]`.  Nothing is patched at run time.

   What remains raw is named in `LEFT_STRUCT_SLOTS`, still warns on every call
   and still aborts under `VKD3D_THUNK_STRICT=1`.  Generation FAILS if the two
   sets do not add up to the whole inventory.

   | method | slot | slots | struct | state |
   |---|---|---|---|---|
   | `CreateGraphicsPipelineState` | 10 | 16 | `D3D12_GRAPHICS_PIPELINE_STATE_DESC` | fixed |
   | `CreateComputePipelineState` | 11 | 16 | `D3D12_COMPUTE_PIPELINE_STATE_DESC` | fixed |
   | `CreatePipelineState` | 47 | 14 | `D3D12_PIPELINE_STATE_STREAM_DESC` (opaque) | fixed (stream walker) |
   | `CopyTextureRegion` | 16 | 11 | `D3D12_TEXTURE_COPY_LOCATION` ×2 per slot | fixed |
   | `ResourceBarrier` | 26 | 11 | `D3D12_RESOURCE_BARRIER` | fixed |
   | `BeginRenderPass` | 68 | 7 | `D3D12_RENDER_PASS_RENDER_TARGET_DESC` + `..._DEPTH_STENCIL_DESC` | fixed |
   | `Barrier` | 80 | 4 | `D3D12_BARRIER_GROUP` | fixed |
   | `LoadGraphicsPipeline` / `LoadComputePipeline` / `LoadPipeline` | 9 / 10 / 13 | 2 / 2 / 1 | pipeline descs / stream | fixed |
   | `CreateStateObject` | 62 | 11 | `D3D12_STATE_OBJECT_DESC` (opaque) | **raw**, STRICT |
   | `AddToStateObject` | 66 | 9 | `D3D12_STATE_OBJECT_DESC` (opaque) | **raw**, STRICT |
   | `GetAutoBreadcrumbsOutput` | 3 | 1 | `D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT` (OUT direction) | **raw**, STRICT |

   The two that are left are left for a reason, not for time:
   `D3D12_STATE_OBJECT_DESC`'s subobject array is self-referential — a
   `SUBOBJECT_TO_EXPORTS_ASSOCIATION` points at a SIBLING subobject, so a
   copied array needs pointer rebasing, not just member rewriting — and the
   CP2077 acceptance run is RT-off.  DRED is the OUT direction: the host writes
   host pointers into a guest-visible node chain for post-mortem tooling, never
   on the frame loop.

   Three things about the fixups are weaker than the rest of this thunk and are
   deliberate:

   * **Unwrap verifies, and that costs a lock.**  A member found inside a
     caller-built struct has no provenance, so the fixups use
     `vkd3d_thunk_unwrap()` (shard mutex + hash lookup, answers "not ours"
     rather than guessing) and not `vkd3d_proxy_unwrap()` (one load, trusts its
     argument).  That is one lock per resource pointer on `ResourceBarrier`,
     the hottest call in the API.  MEASURED on this x86-64 box, uncontended,
     20M calls each: **8.8 ns verifying vs 1.1 ns trusting, so +7.7 ns per
     interface member** — about 250 ns added to a 32-barrier call, against a
     FEX crossing that costs orders of magnitude more.  Nothing was optimised,
     because the measurement says there is nothing to buy.  What is NOT
     measured is behaviour under real multi-threaded command recording, where
     the shard lock could contend; that needs a game.  Any cheaper scheme
     changes the interning table's contract, not this file.
   * **`VKD3D_FIXUP_STREAM_MAX` is 1 MiB.**  `CreatePipelineState`'s
     `SizeInBytes` is guest-supplied and otherwise unbounded; a real stream is
     tens to a few hundred bytes.  Over the cap the call is refused with
     `E_INVALIDARG` **without crossing** — which is a behaviour difference from
     Windows if some future thing ships a 2 MiB stream.
   * **A foreign (non-proxy) member is forwarded UNCHANGED**, with one warning
     per slot, because a raw host pointer arriving over the dxvk-dxgi seam is
     legal.  That means a genuinely bogus pointer is also forwarded, and vkd3d
     gets to be the one that notices.

2. **Three refused slots.**  They warn once by name and return `0` /
   `E_NOTIMPL` **without crossing**:
   `ID3D12WorkGraphProperties::GetNodeIndex` and `::GetEntrypointIndex` take a
   16-byte `D3D12_NODE_ID` **by value**, which no single `uint64_t` slot can
   carry under any of the three ABIs; `ID3DDestructionNotifier::RegisterDestructionCallback`
   takes a guest function pointer vkd3d would call from a host thread, which FEX
   forbids (the boundary analysis §3 already fixes this policy for callbacks).

3. **Application-implemented interfaces are not detected.**
   `ID3D12Object::SetPrivateDataInterface` and
   `ID3D12Device5::CreateLifetimeTracker(ID3D12LifetimeOwner*)` unwrap their
   argument as if it were one of our proxies.  A game that passes its **own**
   implementation there hands vkd3d a guest vtable.  dxvk-ppc64le ships the same
   hole for D3D11; nothing in a C signature distinguishes the two cases.
   `vkd3d_thunk_unwrap()`, which other projects call, does check the table.

4. **`GetPrivateData` with an interface GUID** returns raw host pointer bytes
   through a `void*` buffer.  Not statically detectable, so not flagged either.

5. **The pump is one global queue, not one per device.**  `PUMP_WAIT`'s device
   cookie is accepted and ignored.  The boundary contract permits it (the PE
   shim's per-device pump threads can share one queue), the cost is that one
   device cannot be shut down independently of another, and making it per-device
   is a change to `vkd3d_thunk_host_rt.cpp` alone.

6. **`ID3D12Resource::Map`'s `void**` is flagged `RAW_VOID` but deliberately NOT
   reported by `vkd3d_slot_untranslated()`** — a divergence from the D3D11
   reference, argued in the comment on that function: mapped memory under a
   shared address space is correct by design, unlike dxvk's `GetDecoderBuffer`,
   whose correctness had never been established.

## What has NOT been exercised

- **No FEX boundary has been crossed.**  `vkd3d_thunk_call{,_float,_entry}` are
  undefined in the guest library and are satisfied by the test rig.  Everything
  here is same-process.
- **No real vkd3d-proton.**  The host half's `dlopen`/`dlsym` path compiles and
  is bounds-checked but has never resolved a real symbol here; the D3D11 project
  had its native libraries on the same box and this one does not yet.  Point
  `VKD3D_THUNK_D3D12_LIB` at a real build on the POWER box to close that.
- **ppc64le has not been compiled here.**  The host half is plain portable
  C++17 with no architecture-conditional code outside the `#ifdef __x86_64__`
  `ms_abi` sections; build it on the POWER box with `tests/build.sh`.
- **Coverage is 272 assertions, not coverage of 2343 methods.**  One exemplar
  per marshalling shape, plus the runtime core, plus one per struct fixup --
  and for the fixups, "exemplar" means one call per union arm, not one call per
  method.
- **No real pipeline stream has been through the walker.**  The streams the
  tests build are hand-laid-out and hand-checked against the compiler's own
  `sizeof`/`offsetof` for the four subobject types they use; the other 24 types
  are covered only by that same macro, transcribed from `libs/vkd3d/state.c`.
  The first real evidence is a game that builds pipelines through
  `CreatePipelineState`.
- **The verifying unwrap was measured in isolation, not under contention.**
  +7.7 ns per interface member on an idle box; what a game recording command
  lists on eight threads does to that shard lock is not known.
