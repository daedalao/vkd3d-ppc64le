/*
$info$
tags: thunklibs|vkd3d_d3d12
$end_info$
*/

/* Host (native ppc64le) half of the vkd3d-proton D3D12 FEX thunk library.
 *
 * FEX dlopens this as <FEX_THUNKHOSTLIBS>/libvkd3d_d3d12-host.so when the
 * guest library's LOAD_LIB constructor fires
 * (Source/Tools/LinuxEmulation/Thunks.cpp).
 *
 * Like the guest half, this is thin. The dlopen of native vkd3d-proton, the
 * eight flat entry points, the generic slot dispatcher, the float-class
 * prototypes, the ID3D12Fence::SetEventOnCompletion override and the fence
 * reaper are ppc64le/thunk/runtime/ and ppc64le/thunk/generated/, compiled
 * into this library unmodified (build.sh). All that is here is the four
 * custom_host_impl forwarders.
 *
 * They are nearly nothing, because the thunked surface passes the argument
 * block by ADDRESS: FEX shares one address space, so the guest's uint64_t
 * args[] is directly readable and writable here and no unflattening is
 * needed. dxvk-ppc64le's equivalent rebuilds a uint64_t[10] from ten scalar
 * parameters; this file's forwarders are a cast each. See
 * include/vkd3d_thunk_api.h for why the two projects differ.
 *
 * No vkd3d-proton header is included and no vkd3d-proton library is linked --
 * the "always dynamically attached" requirement. The generated loader's dlopen
 * is described in libvkd3d_d3d12_interface.cpp; the runtime's own dlopen,
 * which is the one that matters, is configurable at run time (see
 * ppc64le/thunk/runtime).
 */

#include <cstdint>

#include "vkd3d_thunk_api.h"

#include "common/Host.h"

#include "thunkgen_host_libvkd3d_d3d12.inl"

/* -- the four functions ppc64le/thunk/runtime actually provides -------------
 *
 * They have the SAME C names as the thunked surface declared in
 * vkd3d_thunk_api.h -- vkd3d_host_dispatch, _dispatch_float, _entry, _probe --
 * because vkd3d_thunk_abi.h names the thunkgen surface that way and the
 * runtime implements those names literally
 * [CODE ppc64le/thunk/runtime/vkd3d_thunk_host_rt.cpp:411,420,491 and
 *  ppc64le/thunk/generated/vkd3d_thunk_host.cpp].
 *
 * But the SIGNATURES differ in one respect that matters here and nowhere else:
 * the runtime spells the argument block, the float inputs and the float output
 * as POINTERS, while the thunked declaration must spell them as uint64_t
 * scalars or thunkgen would try to repack the pointees (see
 * include/vkd3d_thunk_api.h). Same symbol, two C types.
 *
 * That is harmless in practice -- a pointer and a uint64_t occupy the same GPR
 * under SysV and ELFv2 alike, which is the premise the entire boundary rests
 * on -- but it must not be papered over with a cast, because a cast would need
 * a declaration and there can only be one. So the runtime's functions are
 * declared here with their TRUE types under distinct C++ names, bound to the
 * real symbols with asm labels. Nothing is punned, nothing is reinterpreted,
 * and if the runtime ever renames one of them the link fails naming it
 * (`--no-undefined` in build.sh).
 *
 * If ppc64le/thunk later changes those four parameters to uint64_t -- which
 * costs it nothing and is what vkd3d_thunk_abi.h's prose already says
 * ("vkd3d_host_dispatch(iface, slot, host, args_ptr)") -- delete the asm
 * labels and call the names directly. */
extern "C" {
uint64_t vkd3d_rt_dispatch(uint32_t iface, uint32_t slot, uint64_t host, uint64_t* args) __asm__("vkd3d_host_dispatch");
uint64_t vkd3d_rt_dispatch_float(uint32_t iface, uint32_t slot, uint32_t shape, uint64_t host, uint64_t* args, const float* fin,
                                 float* fout) __asm__("vkd3d_host_dispatch_float");
uint32_t vkd3d_rt_entry(uint32_t entry, uint64_t* args) __asm__("vkd3d_host_entry");
uint32_t vkd3d_rt_probe(void) __asm__("vkd3d_host_probe");
}

static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_dispatch(uint32_t iface, uint32_t slot, uint64_t host, uint64_t args_ptr) -> uint64_t {
  return vkd3d_rt_dispatch(iface, slot, host, reinterpret_cast<uint64_t*>(args_ptr));
}

static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_dispatch_float(uint32_t iface, uint32_t slot, uint32_t shape, uint64_t host,
                                                                uint64_t args_ptr, uint64_t fin_ptr, uint64_t fout_ptr) -> uint64_t {
  return vkd3d_rt_dispatch_float(iface, slot, shape, host, reinterpret_cast<uint64_t*>(args_ptr), reinterpret_cast<const float*>(fin_ptr),
                                 reinterpret_cast<float*>(fout_ptr));
}

/* Carries the eight flat entry points AND the two pump ops (VKD3D_ENTRY_PUMP_
 * WAIT / _SHUTDOWN). PUMP_WAIT blocks in native code until a fence completes
 * or the pump is shut down, and writes the guest event cookie back through
 * args[1] -- into the PE shim's own stack frame, which is legal because the
 * address space is shared and the guest thread is parked inside this call for
 * the whole duration. */
static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_entry(uint32_t entry, uint64_t args_ptr) -> uint32_t {
  return vkd3d_rt_entry(entry, reinterpret_cast<uint64_t*>(args_ptr));
}

static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_probe() -> uint32_t {
  return vkd3d_rt_probe();
}

EXPORTS(libvkd3d_d3d12)
