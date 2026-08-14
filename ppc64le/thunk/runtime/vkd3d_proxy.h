/* HAND-MAINTAINED -- not generated.
 *
 * The guest-side proxy object and the interning table over it.
 *
 * A proxy is plain data.  It is NOT a host trampoline, and nothing is allocated
 * on the host when one is made:
 *
 *     vtbl   points at one of the 76 STATIC per-interface-type vtables in the
 *            generated guest file.  Two proxies of the same interface share it.
 *     host   the host-side interface pointer.  FEX shares an address space with
 *            the guest, so this is a real pointer, not a handle table index --
 *            but the guest must never dereference it.
 *     refs   guest-local reference count.  AddRef/Release never cross.
 *     iface  which of the 76 interfaces this proxy presents, so Release and
 *            QueryInterface can dispatch without a per-object trampoline.
 *
 * The whole structure is the D3D11 project's, unchanged: it is the part of that
 * design that survived a threaded stress test and real DXVK, and D3D12 changes
 * nothing about object identity.  See ppc64le/docs/d3d12-boundary-analysis.md §3.
 */
#pragma once
#include <atomic>
#include <cstdint>

/* The generated header, for the interface ids, the IID table, the float shapes,
 * the argument-block width and the by-value aggregate PODs.  It includes
 * vkd3d_thunk_abi.h, which is the boundary contract itself. */
#include "vkd3d_thunk_ids.h"

struct Proxy {
    const void*           vtbl;
    uint64_t              host;
    std::atomic<uint32_t> refs;
    uint32_t              iface;
};

/* An interface id that is not any of the generated ones. */
#define VKD3D_IFACE_INVALID 0xffffffffu

/* Which calling convention a guest vtable uses.  runtime/vkd3d_thunk_abi.h
 * fixes the policy (default MS-x64 on x86-64 because the deployment target is a
 * PE game, VKD3D_THUNK_ABI=sysv|ms to override, frozen at the first proxy,
 * printed once) and exports the two entry points a caller needs; these ids are
 * how the generated tables are indexed. */
enum VkdGuestAbi : uint32_t {
    VKD3D_ABI_SYSV  = 0,
    VKD3D_ABI_MS    = 1,
    VKD3D_ABI_COUNT = 2
};

extern "C" {

/* Intern a host interface pointer as a guest proxy.
 *
 * OWNERSHIP: wrap() CONSUMES one host reference.  Every producer of a host
 * interface pointer in COM hands you one -- QueryInterface, the Create*
 * methods, GetDevice, the flat entry points -- so the caller always has one to
 * give.  If the pointer is already interned, the surplus host reference is
 * dropped here (one extra crossing, on QueryInterface only).
 *
 * The returned proxy carries one guest reference for the caller. */
void* vkd3d_proxy_wrap(uint64_t host, uint32_t iface);

/* Proxy -> host pointer, for interface-typed IN parameters.  Null-safe.
 *
 * This trusts its argument to be one of ours, exactly as the D3D11 project's
 * does: it is called from generated stubs whose parameter is declared as a
 * D3D12 interface, and every such pointer a game can legally pass came out of
 * this thunk.  The one place that is not true -- an application-implemented
 * ID3D12LifetimeOwner, or its own IUnknown through SetPrivateDataInterface --
 * is recorded in README.md as a known hole rather than papered over here.
 * The cross-runtime entry point vkd3d_thunk_unwrap() does check the table,
 * because there the caller is another project's code. */
uint64_t vkd3d_proxy_unwrap(void* proxy);

/* IUnknown, implemented guest-side.  These three occupy slots 0/1/2 of every
 * one of the generated vtables. */
int32_t  vkd3d_proxy_qi(Proxy* self, const void* riid, void** ppvObject);
uint32_t vkd3d_proxy_addref(Proxy* self);
uint32_t vkd3d_proxy_release(Proxy* self);

/* IID -> interface id, or VKD3D_IFACE_INVALID. */
uint32_t vkd3d_iface_from_iid(const void* riid);

/* Drop one host reference on a host pointer we hold but have no proxy for.
 * Release is slot 2 of every COM vtable, so this dispatches as IUnknown
 * regardless of what the pointer actually is. */
void vkd3d_proxy_host_release(uint64_t host);

/* Number of live interned proxies.  Test hook -- a correct run ends at 0. */
uint32_t vkd3d_proxy_live_count(void);

/* ---- vtables and the guest calling convention --------------------------
 *
 * vkd3d_thunk_vtable(iface) returns the PE-FACING TARGET TABLE: an array of
 * SysV function pointers the x86-64 ms_abi forwarders call through, and the
 * array a slot override must patch.  Patching it therefore affects PE callers
 * only -- the SysV vtable handed to ELF callers is a separate array that is
 * never written.  The next work package (hand-written struct fixups for the
 * VKD3D_SLOT_STRUCT_IFACE slots) is the intended user of this hook.
 *
 * vkd3d_thunk_vtable_for(iface, abi) returns the vtable a proxy must carry.  On
 * anything but x86-64 only VKD3D_ABI_SYSV exists and MS returns nullptr;
 * vkd3d_thunk_abi_available() is the bitmask of the modes that do exist. */
const void* const* vkd3d_thunk_vtable(uint32_t iface);
const void* const* vkd3d_thunk_vtable_for(uint32_t iface, uint32_t abi);
uint32_t vkd3d_thunk_abi_available(void);

/* Active guest ABI mode.  vkd3d_thunk_set_abi() must be called before the first
 * proxy is created; afterwards it refuses and returns 0, because live proxies
 * already carry vtables of the old convention.  vkd3d_thunk_set_abi_sysv() and
 * vkd3d_thunk_abi_is_ms() in vkd3d_thunk_abi.h are the boundary-contract
 * spelling of the same two operations. */
uint32_t vkd3d_thunk_abi(void);
uint32_t vkd3d_thunk_set_abi(uint32_t abi);

/* ---- interface-pointer arrays -----------------------------------------
 *
 * ExecuteCommandLists, SetDescriptorHeaps, MakeResident/Evict and
 * SetEventOnMultipleFenceCompletion pass an array of interface pointers plus a
 * separate count, so the array has to be rebuilt on the way through: guest
 * proxies out, host pointers in.  Small arrays live in the inline buffer -- the
 * common ExecuteCommandLists binds one to eight lists -- and anything larger
 * takes one malloc.
 *
 * On allocation failure the array degrades to null rather than to a short
 * buffer: passing a truncated array to vkd3d would be a host-side overrun.  The
 * failure is reported on stderr, not silent. */
#define VKD3D_IFARRAY_INLINE 16

struct VkdIfArray {
    uint64_t* p;
    uint32_t  n;
    uint64_t  inl[VKD3D_IFARRAY_INLINE];
};

/* IN: unwrap n guest proxies into a host-pointer array.  Returns the host
 * address to pass, or 0 when src is null / n is 0 / allocation failed. */
uint64_t vkd3d_ifarray_in(VkdIfArray* s, void* const* src, uint32_t n);

/* OUT: n zeroed slots for the host to write host pointers into. */
uint64_t vkd3d_ifarray_out(VkdIfArray* s, uint32_t n);

/* OUT: wrap what the host wrote back into dst[0..n).  Each host pointer carries
 * one reference, which wrap() consumes; empty slots become null. */
void vkd3d_ifarray_wrap_out(VkdIfArray* s, void** dst, uint32_t n, uint32_t iface);

void vkd3d_ifarray_free(VkdIfArray* s);

/* A slot whose signature the generator refused.  Logs once per slot and the
 * stub returns a poison value -- never a raw pointer, and never a crossing. */
void vkd3d_thunk_refuse(const char* method, const char* why);

/* A slot that crosses with a struct carrying interface pointers inside it.  The
 * struct passes by pointer with no repacking, so those pointers arrive at
 * native vkd3d as guest Proxy*.  This is the hook the next work package
 * replaces with real fixups; until then every call says so.
 *
 *   VKD3D_THUNK_STRICT=1        abort on the first one (a test drives this)
 *   VKD3D_THUNK_STRUCT_WARN=... every (default) | once | off
 */
void vkd3d_thunk_struct_iface(const char* method, const char* type, int arg);

/* Test hook: how many struct-with-interface warnings have been emitted. */
uint64_t vkd3d_thunk_struct_iface_count(void);

/* ---- the three float-class shapes --------------------------------------
 * Hand-written because the generic path packs every argument into a uint64_t
 * and both ABIs place a float in an FP register.  The prototypes here are the
 * REAL ones; gen_thunk.py checks them against interfaces.json. */
void vkd3d_fstub_ClearDepthStencilView(Proxy* self, VkdCpuDescriptorHandle dsv,
                                       uint32_t flags, float depth,
                                       uint8_t stencil, uint32_t rect_count,
                                       const void* rects);
void vkd3d_fstub_OMSetDepthBounds(Proxy* self, float min_depth, float max_depth);
void vkd3d_fstub_RSSetDepthBias(Proxy* self, float bias, float clamp,
                                float slope_scaled);

}
