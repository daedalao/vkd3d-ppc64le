/*
 * vkd3d_thunk_abi.h — the entire guest<->host boundary for D3D12.
 *
 * D3D12 analog of dxvk-ppc64le's thunk/runtime/dxvk_thunk_abi.h, and
 * deliberately the same shape: a handful of flat functions carry every COM
 * method and every d3d12.dll export, because FEX's host trampoline allocator
 * is a bump allocator with no free path — each distinct thunked symbol is a
 * permanent cost, so the surface is fixed and small.
 *
 * Everything here is integer-register transport. uint64_t slots carry:
 *   - integers and enums (zero/sign extension per the C promotion the stub
 *     compiled against),
 *   - pointers (guest and host share one address space under FEX),
 *   - by-value 8-byte aggregates: D3D12_CPU_DESCRIPTOR_HANDLE {SIZE_T},
 *     D3D12_GPU_DESCRIPTOR_HANDLE {UINT64}, LUID {LONG,DWORD}. MS-x64 passes
 *     any 1/2/4/8-byte aggregate in one GPR, SysV classifies them as one
 *     INTEGER eightbyte, ELFv2 passes them in a GPR: uint64_t is correct
 *     transport for all three ABIs. The census generator must fail if any
 *     by-value parameter larger than 8 bytes ever appears.
 *   - aggregate returns: NOT special. The widl C vtable gives those slots an
 *     explicit `RET *__ret` parameter right after `This`, returning the
 *     pointer — the same convention MSVC-compiled callers use. They ride the
 *     generic path as (this, retptr, args...) -> retptr.
 *
 * Float-class arguments (by-value FLOAT parameters, e.g. Depth in
 * ClearDepthStencilView, Min/Max in OMSetDepthBounds) never enter args[];
 * they use vkd3d_thunk_call_float with a shape id, exactly like dxvk's
 * five-shape design. The generator enumerates shapes and fails on a
 * float-class method it does not know.
 */

#ifndef VKD3D_THUNK_ABI_H
#define VKD3D_THUNK_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interface ids are indices into the generated tables (vkd3d_thunk_ids.h).
 * Slot numbers are the frozen D3D12 vtable slots from the widl headers —
 * specification-stable, not vkd3d-version-stable-only. */

#define VKD3D_THUNK_MAX_ARGS 24u

/* ---- guest -> host: the whole surface ---------------------------------- */

/* Generic COM method crossing. `host` is the host-side interface pointer
 * (the proxy's wrapped pointee). args[] holds every parameter except This,
 * in declaration order, one uint64_t each (see transport rules above).
 * Returns the raw 64-bit return register value. */
uint64_t vkd3d_thunk_call(uint32_t iface, uint32_t slot, uint64_t host,
                          uint64_t *args);

/* Float-class crossing: `shape` selects the exact host-side prototype.
 * fin/fout carry float-class ins and the float return (if any). */
uint64_t vkd3d_thunk_call_float(uint32_t iface, uint32_t slot, uint32_t shape,
                                uint64_t host, uint64_t *args,
                                const float *fin, float *fout);

/* Flat entry points (the d3d12.dll exports) and pump control ops.
 * Returns the entry's HRESULT (or op-specific value) in the low 32 bits. */
uint32_t vkd3d_thunk_call_entry(uint32_t entry, uint64_t *args);

/* Host attach probe: forces the host stub to dlopen native vkd3d-proton and
 * resolve its exports; returns the number resolved (want VKD3D_ENTRY_COUNT
 * of the dlsym'd kind). Callable any number of times. */
uint32_t vkd3d_thunk_host_probe(void);

/* ---- entry ids ---------------------------------------------------------- */

enum vkd3d_thunk_entry {
    /* The eight d3d12.dll flat exports, dlsym'd from native
     * libvkd3d-proton-d3d12.so. Argument layouts documented per entry in the
     * guest-side hand-written wrappers (vkd3d_proxy.cpp). */
    VKD3D_ENTRY_CREATE_DEVICE = 0,             /* (adapter=0, minFL, riid*, void** out) */
    VKD3D_ENTRY_GET_DEBUG_INTERFACE,           /* (riid*, void** out) */
    VKD3D_ENTRY_GET_INTERFACE,                 /* (rclsid*, riid*, void** out) */
    VKD3D_ENTRY_CREATE_ROOT_SIG_DESERIALIZER,  /* (data*, size, riid*, void** out) */
    VKD3D_ENTRY_CREATE_VERSIONED_RS_DESER,     /* (data*, size, riid*, void** out) */
    VKD3D_ENTRY_SERIALIZE_ROOT_SIG,            /* (desc*, version, blob** out, errblob** out) */
    VKD3D_ENTRY_SERIALIZE_VERSIONED_ROOT_SIG,  /* (desc*, blob** out, errblob** out) */
    VKD3D_ENTRY_ENABLE_EXPERIMENTAL_FEATURES,  /* (count, iids*, configs*, sizes*) */
    VKD3D_ENTRY_COUNT_DLSYM,                   /* number of dlsym'd entries above */

    /* Pump ops — implemented by the host stub itself, not by vkd3d.
     * See "fence/event pump" below. */
    VKD3D_ENTRY_PUMP_WAIT = 32,   /* args[0]=device cookie; blocks host-side;
                                     returns 1 and fills args[1]=guest event
                                     cookie, or returns 0 on pump shutdown */
    VKD3D_ENTRY_PUMP_SHUTDOWN,    /* args[0]=device cookie; wakes PUMP_WAIT
                                     with shutdown */
};

/* ---- fence/event pump ---------------------------------------------------
 *
 * Problem: ID3D12Fence::SetEventOnCompletion(value, hEvent) — hEvent is a
 * guest Win32 HANDLE, signaled by vkd3d's internal worker threads (host
 * pthreads). FEX forbids host->guest calls outside a guest-initiated
 * crossing, and native vkd3d would interpret the HANDLE bits as an eventfd
 * (vkd3d_native_sync_handle.h). Passing the Wine handle through is wrong on
 * every level (the box runs ntsync, so it is not even an eventfd).
 *
 * Resolution: the host DISPATCHER intercepts (ID3D12Fence*, slot
 * SetEventOnCompletion) — a slot-override table keyed (iface, slot), the
 * only entries in an otherwise fully generic dispatcher:
 *
 *   hEvent == NULL:  pass through unchanged. Native vkd3d blocks the calling
 *                    thread on a condvar until the value completes
 *                    (command.c:d3d12_fence_set_native_sync_handle_on_
 *                    completion_explicit) — the D3D12 blocking-wait
 *                    contract, correct across a synchronous crossing.
 *
 *   hEvent != NULL:  the override allocates a pooled eventfd E (never fd 0 —
 *                    vkd3d treats fd 0 as invalid-by-design), calls the real
 *                    slot with E as the HANDLE, and hands (E, cookie=hEvent
 *                    bits) to the per-device reaper thread. The reaper (host
 *                    stub's own pthread; native code, free to block) polls
 *                    pending E's; on completion it recycles E into the pool
 *                    and pushes the cookie onto the completion queue,
 *                    signaling the doorbell.
 *
 * Guest side: the PE shim creates ONE pump thread per device (Win32
 * CreateThread). It loops on VKD3D_ENTRY_PUMP_WAIT (a synchronous crossing
 * that blocks host-side on the doorbell), and for each returned cookie calls
 * Win32 SetEvent((HANDLE)cookie) in guest code. All host->guest transitions
 * are therefore returns from guest-initiated calls — FEX-legal.
 *
 * ID3D12Device1::SetEventOnMultipleFenceCompletion gets the same override.
 * GetCompletedValue stays generic (a plain read crossing).
 *
 * Ordering note: two SetEventOnCompletion registrations completing in quick
 * succession may be observed by the guest in either order — permitted by the
 * D3D12 contract (distinct events; apps synchronize on the event, not on
 * signal order).
 */

/* ---- host-side exports (thunkgen surface) -------------------------------
 * The FEX-thunked host functions, all custom_host_impl, flattened to
 * scalars exactly like dxvk's four:
 *
 *   vkd3d_host_dispatch(iface, slot, host, args_ptr)       -> uint64
 *   vkd3d_host_dispatch_float(iface, slot, shape, host, args_ptr, fin_ptr, fout_ptr) -> uint64
 *
 * Census fact [MEASURED 2026-08-13]: the widest slot in the whole surface is
 * exactly 10 parameters (ID3D12Device*::CreateCommittedResource3), so the
 * dxvk-style uint64_t[10] argument block fits D3D12 with no widening.
 *
 * Census fact: exactly two slots take a BY-VALUE aggregate larger than
 * 8 bytes — ID3D12WorkGraphProperties::GetNodeIndex and ::GetEntrypointIndex
 * (D3D12_NODE_ID, a 16-byte {LPCWSTR,UINT}). MS-x64 passes those by hidden
 * reference, SysV/ELFv2 in two registers: single-slot transport is WRONG for
 * them. They are REFUSED by the generator (stub returns a poison value and
 * warns once); work graphs are outside every target workload. Everything
 * else by-value is <= 8 bytes.
 *   vkd3d_host_entry(entry, args_ptr)                      -> uint32
 *   vkd3d_host_probe()                                     -> uint32
 */

/* ---- ABI mode ------------------------------------------------------------
 * Same three-array design as dxvk (sysv vtbl / target[] / ms_abi vtbl),
 * default MS-x64 on x86-64 builds, VKD3D_THUNK_ABI=sysv|ms override,
 * frozen at first proxy, printed once to stderr. ELF-guest tests must set
 * VKD3D_THUNK_ABI=sysv. */

void vkd3d_thunk_set_abi_sysv(void);   /* refuses after first proxy */
int  vkd3d_thunk_abi_is_ms(void);

/* ---- cross-runtime interop (dxvk-dxgi native seam) -----------------------
 * dxvk's DXGI shim must hand NATIVE dxvk-dxgi the HOST pointer under our
 * ID3D12CommandQueue proxy (CreateSwapChainForHwnd), and may need to wrap a
 * host pointer it got from vkd3d natively. Exported with C linkage and a
 * probe so their guest .so can dlsym us:
 *
 *   vkd3d_thunk_unwrap(p)   -> host pointer if p is one of our live proxies,
 *                              else 0 (never guesses).
 *   vkd3d_thunk_wrap(host, iface_id) -> proxy (interning table; consumes one
 *                              host reference, same ownership rule as QI).
 *   vkd3d_thunk_interop_version() -> 1.
 */
uint64_t vkd3d_thunk_unwrap(void *maybe_proxy);
void    *vkd3d_thunk_wrap(uint64_t host, uint32_t iface_id);
uint32_t vkd3d_thunk_interop_version(void);

#ifdef __cplusplus
}
#endif

#endif /* VKD3D_THUNK_ABI_H */
