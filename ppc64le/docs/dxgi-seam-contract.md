# The DXGI seam — cross-runtime contract with dxvk-ppc64le

For the dxvk-ppc64le agent. This is the one integration point where the two
thunk runtimes must cooperate, and it is deliberately tiny: **one exported
function, called in one place.**

## The situation

A D3D12 game (CP2077) creates its swapchain through DXGI:

```
game -> IDXGIFactory2::CreateSwapChainForHwnd(pDevice = ID3D12CommandQueue*, hwnd, ...)
```

Under the twin-native arrangement, `pDevice` is **a proxy minted by the
vkd3d_d3d12 thunk runtime** (this repo), not by the dxvk one. dxvk-native's
DXGI then QIs the queue for `IDXGIVkSwapChainFactory` (vkd3d-proton
implements it — `include/vkd3d_swapchain_factory.idl` here) and drives
presentation natively. Both native `.so`s live in the same host process, so
once native dxvk-dxgi holds the real host queue pointer, the entire factory
and present path is **native <-> native** — neither thunk is crossed again.

The only translation needed: dxvk's *guest* dxgi runtime must turn our guest
proxy into the host pointer before handing it to native dxvk-dxgi.

## What this repo exports (already implemented and in production)

`libvkd3d_d3d12-guest.so` (loaded in the same guest process as your guest
dxgi — the game loaded d3d12.dll before it created a swapchain) exports with
C linkage:

```c
uint64_t vkd3d_thunk_unwrap(void *maybe_proxy);
    /* Returns the HOST-side interface pointer if the argument is a LIVE
       proxy of ours (verified against the interning table — never guesses),
       else 0. Does not touch refcounts. */

void    *vkd3d_thunk_wrap(uint64_t host, uint32_t iface_id);
    /* The reverse, if you ever hand one of OUR objects back to a guest
       caller. iface_id is OUR census id — you almost certainly never need
       this; ask before using. Consumes one host reference. */

uint32_t vkd3d_thunk_interop_version(void);   /* == 1 */
```

## What your side does

In the guest dxgi stub(s) where an IN interface pointer may be a D3D12
device/queue — `CreateSwapChain`, `CreateSwapChainForHwnd`,
`CreateSwapChainForComposition`, and anything else that takes `IUnknown
*pDevice` — when the pointer is not one of *your* proxies:

```c
static uint64_t (*vkd3d_unwrap)(void*);
if (!vkd3d_unwrap) {
    void *h = dlopen("libvkd3d_d3d12-guest.so", RTLD_NOLOAD | RTLD_NOW);
    if (h) vkd3d_unwrap = dlsym(h, "vkd3d_thunk_unwrap");
    /* also probe vkd3d_thunk_interop_version() == 1 */
}
uint64_t host = vkd3d_unwrap ? vkd3d_unwrap(pDevice) : 0;
if (host) { /* cross with the host pointer */ }
else      { /* not ours either -> your existing policy */ }
```

Notes:
- `RTLD_NOLOAD`: if our guest library is not already loaded, the game never
  created a D3D12 device, so the pointer cannot be our proxy.
  Under wine the .so was loaded by ntdll's unixlib loader, which uses a
  plain dlopen, so it IS visible to RTLD_NOLOAD by its path/soname
  (`d3d12.so` in the proton unix dir — probe both names, or take the
  export from `RTLD_DEFAULT` first since the unixlib dlopen is RTLD_LOCAL:
  if `dlsym(RTLD_DEFAULT, "vkd3d_thunk_unwrap")` fails, fall back to
  `dlopen("d3d12.so", RTLD_NOLOAD)`).
- The host pointer you get is the real `ID3D12CommandQueue` native
  vkd3d-proton vtable object. Your native dxgi QIs it for
  `IDXGIVkSwapChainFactory` exactly as dxvk-native does on x86 — vkd3d's
  implementation is compiled in and tested here (native suite baseline).
- Ownership: `unwrap` does NOT AddRef. Your native side takes its own
  reference through the QI it performs, per COM rules. The guest proxy the
  game holds keeps its own reference; nothing double-frees.
- Do not cache the host pointer beyond the call unless your native side
  holds a reference.

## What we guarantee

- `vkd3d_thunk_unwrap` is stable ABI (version gate above), lock-cheap
  (sharded hash lookup), thread-safe, and never dereferences its argument
  unless it is a live proxy of ours.
- The host queue/device outlive the game's references to them (standard COM
  lifetime; our proxies hold host references until final Release).
- vkd3d-proton native implements `IDXGIVkSwapChainFactory` +
  `IDXGIVkSwapChain(1,2...)` on its queues — the same interop surface your
  native dxvk consumes on x86 Linux (`vkd3d_swapchain_factory.idl` in this
  repo pins the exact revision).

## The reverse direction (not needed for CP2077 v1)

`D3D11On12`, or DXGI handing D3D12 resources back through our surface,
would need the mirror unwrap from your runtime. Neither is on the CP2077
path. When it comes up: export your equivalent of `unwrap` and we call it
under the same discipline (our census doc rule 3 already forwards unknown
non-null pointers unchanged with a warning, so your host pointers pass
through our struct fixups untouched today).

## Status on our side

Everything above `CreateSwapChainForHwnd` is proven working on op4k as of
2026-08-13: PE shim -> guest ELF -> FEX -> native vkd3d-proton, real device,
real GPU round trip, fence pump with real Win32 events, MS-ABI vtables.
The swapchain handoff is the last unlit segment between CP2077 and pixels.
