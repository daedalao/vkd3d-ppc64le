# Struct-with-interface fixups — the census and the plan

dxvk-ppc64le could pass every aggregate by pointer untouched except three
video-path slots it deliberately punted (its docs §8.1). D3D12 does not allow
that luxury: interface pointers hide inside structs on the hottest paths in
the API. This is the one place the D3D11 design is extended rather than
ported. Inventory below is mechanical (scan of widl header struct bodies ×
interfaces.json param types, closure over by-value embedding), 2026-08-13.

## Slots needing guest-side member unwrap (IN direction)

| slot(s) | struct | members to unwrap | heat |
|---|---|---|---|
| `ResourceBarrier` (GCL0–10) | `D3D12_RESOURCE_BARRIER[]` | union by `Type`: `Transition.pResource`, `Aliasing.pResourceBefore/After`, `UAV.pResource` | **hottest call in the API** |
| `CopyTextureRegion` (GCL0–10) | 2× `D3D12_TEXTURE_COPY_LOCATION` | `.pResource` each | hot |
| `CreateGraphicsPipelineState`, `CreateComputePipelineState` (Device0–15), `LoadGraphicsPipeline`, `LoadComputePipeline` (PipelineLibrary0–1) | `*_PIPELINE_STATE_DESC` | `.pRootSignature` | load-time burst (CP2077: thousands) |
| `CreatePipelineState` (Device2–15) | `D3D12_PIPELINE_STATE_STREAM_DESC` | **invisible to the type scan** — the `void*` subobject stream contains `..._ROOT_SIGNATURE` subobjects; needs a stream walker (copy stream to scratch, iterate `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE` tokens with pointer-size alignment, rewrite root-signature payloads) | load-time |
| `BeginRenderPass` (GCL4–10) | `D3D12_RENDER_PASS_RENDER_TARGET_DESC[]`, `D3D12_RENDER_PASS_DEPTH_STENCIL_DESC` | `EndingAccess.Resolve.pSrcResource/pDstResource` (color + depth flavors) | per-pass |
| `Barrier` (GCL7–10, enhanced barriers) | `D3D12_BARRIER_GROUP[]` → `D3D12_TEXTURE_BARRIER[]`/`D3D12_BUFFER_BARRIER[]` | `.pResource` per barrier (GLOBAL groups carry none) | hot if EB used (CP2077 uses legacy) |
| `CreateStateObject`, `AddToStateObject` (Device5+) | `D3D12_STATE_OBJECT_DESC` | `D3D12_GLOBAL/LOCAL_ROOT_SIGNATURE.pGlobalRootSignature`, `D3D12_EXISTING_COLLECTION_DESC.pExistingCollection`; **self-referential**: `SUBOBJECT_TO_EXPORTS_ASSOCIATION.pSubobjectToAssociate` points at sibling subobjects, so a copied array needs pointer rebasing | **deferred**: STRICT-flagged passthrough until the RT milestone; CP2077 acceptance run is RT-off |

## OUT direction (host writes host pointers into guest-visible structs)

DRED only: `GetAutoBreadcrumbsOutput` / breadcrumb + allocation nodes embed
`ID3D12CommandQueue*`/`ID3D12GraphicsCommandList*`/`IUnknown*` written by the
host for post-mortem tooling. Diagnostics-path, never on the frame loop;
passthrough with STRICT note. A debugger reading them sees host pointers —
acceptable and documented.

## Design rules for the fixup module (`runtime/vkd3d_struct_fixups.cpp`, hand-written)

1. Hooked through the same `target[]` override mechanism as the float slots —
   generated tables stay generic; overrides installed at init; the generated
   STRUCT_IFACE STRICT warning must go silent for every slot the module
   claims (the install function returns the count, tests assert it).
2. Scratch: inline stack arrays sized for the common case (32 barriers, 8 RT
   descs), heap fallback above, degrade to abort-with-message on OOM — never
   a short copy.
3. Unwrap = `vkd3d_thunk_unwrap` semantics: a member that is not a live proxy
   of ours passes through UNCHANGED if null, and is a hard error (STRICT
   abort, loud warn otherwise) if non-null and unknown — a raw host pointer
   from cross-runtime interop is legal (dxvk-dxgi seam), so unknown non-null
   values are forwarded with a once-per-slot warning rather than rejected.
4. The copied struct lives only for the crossing; D3D12 semantics for all
   these slots are copy-in (the runtime may not retain the pointer), which is
   what makes the copy legal. `CreatePipelineState`'s stream is copy-in too.
5. Alignment: scratch copies use `alignas(void*)` raw buffers; the stream
   walker must respect widl's subobject alignment rule (each subobject starts
   at `alignof(void*)`).
