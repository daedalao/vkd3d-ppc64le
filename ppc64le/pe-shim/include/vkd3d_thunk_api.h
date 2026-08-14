/* The host-side API that FEX thunks across the guest->host boundary.
 *
 * This is the file thunkgen parses (via libvkd3d_d3d12_interface.cpp). It is a
 * one-to-one restatement of the four host-side functions named in
 * ppc64le/thunk/runtime/vkd3d_thunk_abi.h, section "host-side exports
 * (thunkgen surface)":
 *
 *   vkd3d_host_dispatch(iface, slot, host, args_ptr)                    -> u64
 *   vkd3d_host_dispatch_float(iface, slot, shape, host, args, fin, fout)-> u64
 *   vkd3d_host_entry(entry, args_ptr)                                   -> u32
 *   vkd3d_host_probe()                                                  -> u32
 *
 * -- Why every parameter here is a scalar -----------------------------------
 *
 * thunkgen repacks any pointee it is shown, and it has no way to know an
 * array's length. So no pointer TYPE appears in this file: the argument block,
 * the float-in block and the float-out slot all cross as plain uint64_t
 * ADDRESSES. That is sound because FEX gives guest and host one address space
 * (vkd3d_thunk_abi.h, transport rules), and it is what makes thunkgen emit
 * zero repacking code for this library -- the same deliberate design dxvk-
 * ppc64le's pe-shim/include/dxvk_thunk_api.h documents and measures.
 *
 * It also matters for more than tidiness: FEX's host trampoline allocator is a
 * bump allocator with no free path (Source/Tools/LinuxEmulation/Thunks.cpp),
 * and every thunked function is a permanent SHA-256 export record. Four is a
 * fixed cost; the ~7000 D3D12 vtable slots would not have been.
 *
 * -- Divergence from the dxvk reference, and why ----------------------------
 *
 * dxvk's four functions FLATTEN the argument array into ten separate uint64_t
 * parameters and pass the two float-class inputs by value. This file passes
 * ONE uint64_t holding the address of the guest's uint64_t args[] instead,
 * because vkd3d_thunk_abi.h specifies the boundary that way (`uint64_t *args`,
 * VKD3D_THUNK_MAX_ARGS 24) and the census fact recorded there -- widest slot
 * is exactly 10 parameters -- is a measurement of today's surface, not a
 * guarantee for tomorrow's. Passing the address costs one indirection on the
 * host side and makes the arity census a host-side concern only; flattening
 * would have frozen the arity into the thunked ABI, where widening it later
 * means a new SHA-256 export record and a FEX rebuild.
 *
 * The float shapes ride the same rule: `fin`/`fout` cross as addresses, so a
 * shape needing three floats in or two out never touches this file.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic COM vtable dispatch. `host` is the HOST interface pointer -- the
 * guest proxy has already been unwrapped by the guest runtime. `args_ptr` is
 * the address of the guest's uint64_t args[], holding every parameter except
 * `This` in declaration order (aggregate-return slots include the explicit
 * `__ret` pointer as args[0], exactly as the widl C vtable declares it).
 * Returns the raw 64-bit return register value. */
uint64_t vkd3d_host_dispatch(uint32_t iface, uint32_t slot, uint64_t host, uint64_t args_ptr);

/* Float-class dispatch. `shape` selects the exact host-side prototype;
 * `fin_ptr` is the address of the guest's `const float[]` inputs (may be 0),
 * `fout_ptr` the address of the guest's float return slot (may be 0). Returns
 * the integer-class return value, as vkd3d_thunk_call_float does. */
uint64_t vkd3d_host_dispatch_float(uint32_t iface, uint32_t slot, uint32_t shape, uint64_t host, uint64_t args_ptr, uint64_t fin_ptr,
                                   uint64_t fout_ptr);

/* One of the eight flat d3d12.dll entry points, or a pump op -- see
 * enum vkd3d_thunk_entry in vkd3d_thunk_abi.h. `args_ptr` is the address of
 * the guest's uint64_t args[]; PUMP_WAIT writes its result back into
 * args[1] through that same pointer. Returns the HRESULT (or the op-specific
 * value) in the low 32 bits. */
uint32_t vkd3d_host_entry(uint32_t entry, uint64_t args_ptr);

/* Force the host-side dlopen of native vkd3d-proton now and report how many of
 * the eight flat entry points resolved. Called once at attach so "vkd3d-proton
 * is not installed" surfaces as a named diagnostic rather than as a null call
 * at the first CreateDevice. */
uint32_t vkd3d_host_probe(void);

#ifdef __cplusplus
}
#endif
