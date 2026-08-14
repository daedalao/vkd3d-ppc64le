/*
$info$
tags: thunklibs|vkd3d_d3d12
$end_info$
*/

/* Guest (x86-64 ELF) half of the vkd3d-proton D3D12 FEX thunk library.
 *
 * This file is deliberately thin. The COM runtime -- proxy interning,
 * reference counting, QueryInterface, the eight flat entry points, the
 * generated vtable stubs -- is ppc64le/thunk/runtime/ and
 * ppc64le/thunk/generated/, and is compiled into this library unmodified. What
 * is missing from it, and all that is here, is the four guest-side functions
 * of ppc64le/thunk/runtime/vkd3d_thunk_abi.h: the actual crossing.
 *
 * Two things beyond that crossing:
 *
 *  - LOAD_LIB_INIT(libvkd3d_d3d12, ...) makes FEX dlopen the matching host
 *    library, <FEX_THUNKHOSTLIBS>/libvkd3d_d3d12-host.so
 *    (Source/Tools/LinuxEmulation/Thunks.cpp, {ThunkHostLibsPath}/{Name}-host.so).
 *
 *  - __wine_unix_call_funcs makes this library loadable by a PE d3d12.dll.
 *    That is the seam the project does not otherwise have: FEX's ThunksDB
 *    overlays guest ELF .so files at dlopen, and nothing under a PE d3d12.dll
 *    is an ELF .so. Wine's unixlib mechanism gives a PE module a sanctioned
 *    way to dlopen one and get a function table back, so we present ourselves
 *    as one.
 *
 * -- The constraint the vtable stubs must satisfy --------------------------
 *
 * After D3D12CreateDevice returns, the game calls proxy->vtbl[n] directly from
 * PE code. There is no unix-call dispatcher on that path and the thread is
 * running with a Windows TEB, so a stub must not touch thread-local storage,
 * errno, or anything that can block. The generated stubs satisfy this: each is
 * a uint64_t[] on the stack and one 0F 3F. Nothing enforces it, which is why
 * it is written down here.
 *
 * The PE shim's fence pump thread is subject to the same rule and is the one
 * caller that BLOCKS on purpose: it enters vkd3d_thunk_call_entry(PUMP_WAIT)
 * and stays inside the host until a fence completes. That is legal because it
 * blocks in HOST code on a host doorbell, having entered through an ordinary
 * guest-initiated crossing -- no host->guest call is ever made outside a
 * return. See vkd3d_thunk_abi.h, "fence/event pump".
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

/* The boundary definition, owned by ppc64le/thunk/runtime/. Declares the four
 * guest functions this file defines. */
#include "vkd3d_thunk_abi.h"

/* The thunked host surface, owned by this directory. */
#include "vkd3d_thunk_api.h"
#include "vkd3d_unixlib.h"

#include "common/Guest.h"

#include "thunkgen_guest_libvkd3d_d3d12.inl"

/* ------------------------------------------------------------------------ */
/* The crossing.                                                             */
/* ------------------------------------------------------------------------ */

/* Every one of these is a straight widening of a pointer to a uint64_t. The
 * address space is shared under FEX, so the host dereferences the guest's
 * args[] in place; sending the address rather than the elements is what keeps
 * the thunked signature pointer-free and therefore repack-free
 * (include/vkd3d_thunk_api.h).
 *
 * All four carry an explicit default-visibility attribute. The library is
 * built -fvisibility=hidden (build.sh), and these four are the abi header's
 * declared guest surface: tests/attach.c dlsym()s them, and a second runtime
 * doing cross-runtime interop would too. dxvk-ppc64le could leave its
 * equivalents hidden because nothing outside the .so ever asked for their
 * addresses; here the PE shim's pump needs vkd3d_thunk_call_entry, so leaving
 * them hidden would have been a null pointer in the init table rather than a
 * compile error. */
#define BOUNDARY extern "C" __attribute__((visibility("default")))

BOUNDARY uint64_t vkd3d_thunk_call(uint32_t iface, uint32_t slot, uint64_t host, uint64_t* args) {
  return fexfn_pack_vkd3d_host_dispatch(iface, slot, host, reinterpret_cast<uint64_t>(args));
}

BOUNDARY uint64_t vkd3d_thunk_call_float(uint32_t iface, uint32_t slot, uint32_t shape, uint64_t host, uint64_t* args, const float* fin,
                                         float* fout) {
  return fexfn_pack_vkd3d_host_dispatch_float(iface, slot, shape, host, reinterpret_cast<uint64_t>(args), reinterpret_cast<uint64_t>(fin),
                                              reinterpret_cast<uint64_t>(fout));
}

BOUNDARY uint32_t vkd3d_thunk_call_entry(uint32_t entry, uint64_t* args) {
  return fexfn_pack_vkd3d_host_entry(entry, reinterpret_cast<uint64_t>(args));
}

/* The abi header lists vkd3d_thunk_host_probe() on the guest->host surface. It
 * is defined HERE because the probe is nothing but a crossing: there is no
 * runtime state involved. If ppc64le/thunk ever defines it too, delete this
 * one -- the linker will say so. */
BOUNDARY uint32_t vkd3d_thunk_host_probe(void) {
  return fexfn_pack_vkd3d_host_probe();
}

/* ------------------------------------------------------------------------ */
/* Wine unixlib surface.                                                     */
/* ------------------------------------------------------------------------ */

extern "C" {

/* Index-matched to VKD3D_PE_* / the first eight members of enum
 * vkd3d_thunk_entry. Resolved by NAME rather than by taking addresses so that
 * the eight prototypes -- which belong to ppc64le/thunk/runtime's proxy
 * layer -- are not duplicated here where they could drift. RTLD_DEFAULT finds
 * them because they are exported from this same object with default
 * visibility. */
static const char* const kEntryName[VKD3D_PE_ENTRY_COUNT] = {
  "D3D12CreateDevice",
  "D3D12GetDebugInterface",
  "D3D12GetInterface",
  "D3D12CreateRootSignatureDeserializer",
  "D3D12CreateVersionedRootSignatureDeserializer",
  "D3D12SerializeRootSignature",
  "D3D12SerializeVersionedRootSignature",
  "D3D12EnableExperimentalFeatures",
};

/* The boundary functions get their addresses taken DIRECTLY rather than
 * dlsym'd: all four are defined a few lines above, in this same translation
 * unit, so there is nothing to look up and nothing that can drift. Only
 * vkd3d_thunk_set_abi_sysv belongs to ppc64le/thunk's runtime, and it is
 * OPTIONAL -- an ELF-only build may not have it, and the PE never calls it
 * (a PE caller wants MS-x64, which is the default on x86-64). A null in that
 * slot is therefore not an error; a null in any other would be a bug in this
 * file. */
static uint64_t BoundaryAddress(unsigned i) {
  switch (i) {
  case VKD3D_PE_BOUNDARY_CALL: return reinterpret_cast<uint64_t>(&vkd3d_thunk_call);
  case VKD3D_PE_BOUNDARY_CALL_FLOAT: return reinterpret_cast<uint64_t>(&vkd3d_thunk_call_float);
  case VKD3D_PE_BOUNDARY_CALL_ENTRY: return reinterpret_cast<uint64_t>(&vkd3d_thunk_call_entry);
  case VKD3D_PE_BOUNDARY_HOST_PROBE: return reinterpret_cast<uint64_t>(&vkd3d_thunk_host_probe);
  case VKD3D_PE_BOUNDARY_SET_ABI_SYSV: return reinterpret_cast<uint64_t>(dlsym(RTLD_DEFAULT, "vkd3d_thunk_set_abi_sysv"));
  default: return 0;
  }
}

static int unixcall_init(void* argsv) {
  auto* p = static_cast<vkd3d_unix_init_params*>(argsv);

  /* Refuse to write past a caller whose struct is older/shorter than ours.
   * Both halves come out of one build.sh run, so this is belt-and-braces --
   * but a wild write into a PE's static data is not a failure mode worth
   * having, and the magic is what says the `in` fields exist at all. */
  if (p->in_magic == VKD3D_UNIX_INIT_MAGIC && p->in_size < sizeof(*p)) {
    return 0xC0000004; /* STATUS_INFO_LENGTH_MISMATCH */
  }

  /* Crosses to the host, which dlopens native vkd3d-proton and counts resolved
   * symbols. Doing it here rather than at first call means a missing or broken
   * vkd3d-proton is reported while the PE shim is still able to fail cleanly.
   */
  p->resolved = vkd3d_thunk_host_probe();

  /* Diagnostic. Resolved by name rather than called directly so that a runtime
   * built without the three-array ABI machinery still links: vkd3d_thunk_abi.h
   * declares vkd3d_thunk_abi_is_ms(), but nothing here needs it to exist. */
  {
    auto* abi_is_ms = reinterpret_cast<int (*)(void)>(dlsym(RTLD_DEFAULT, "vkd3d_thunk_abi_is_ms"));
    p->abi_is_ms = abi_is_ms ? static_cast<uint32_t>(abi_is_ms()) : 0u;
  }

  unsigned missing = 0;
  for (unsigned i = 0; i < VKD3D_PE_ENTRY_COUNT; ++i) {
    void* sym = dlsym(RTLD_DEFAULT, kEntryName[i]);
    p->entry[i] = reinterpret_cast<uint64_t>(sym);
    if (!sym) {
      ++missing;
      std::fprintf(stderr, "vkd3d_thunk: guest entry point %s is not exported\n", kEntryName[i]);
    }
  }
  for (unsigned i = 0; i < VKD3D_PE_BOUNDARY_COUNT; ++i) {
    p->boundary[i] = BoundaryAddress(i);
    if (!p->boundary[i] && i != VKD3D_PE_BOUNDARY_SET_ABI_SYSV) {
      ++missing;
      std::fprintf(stderr, "vkd3d_thunk: boundary function %u has no address\n", i);
    }
  }

  /* STATUS_SUCCESS even when vkd3d-proton is absent: the PE side inspects
   * `resolved` and turns that into a proper HRESULT for the caller. A failed
   * NTSTATUS here would be indistinguishable from "the unixlib itself is
   * broken". */
  return missing ? 0xC0000139 /* STATUS_ENTRYPOINT_NOT_FOUND */ : 0;
}

/* The name wine's ntdll dlsym()s after dlopen()ing this file. */
__attribute__((visibility("default"))) const void* __wine_unix_call_funcs[unix_vkd3d_funcs_count] = {
  reinterpret_cast<const void*>(&unixcall_init),
};

} // extern "C"

/* ------------------------------------------------------------------------ */

void OnInit() {
  /* Nothing to register. D3D12 has no app-implemented COM interfaces on any
   * path a game exercises (docs/d3d12-boundary-analysis.md §3), so there are
   * no guest callbacks for the host to invoke and no
   * RegisterGuestCallbackUnpacker<> calls belong here. The one asynchronous
   * host->guest need -- fence event signalling -- is served by the pump
   * (vkd3d_thunk_abi.h §"fence/event pump"), which is a guest-initiated
   * blocking crossing and needs no callback machinery at all.
   *
   * ID3D12InfoQueue1::RegisterMessageCallback is the single genuine
   * host-thread callback in the API. It is debug-layer only and the boundary
   * analysis rules it accept-and-warn / E_NOTIMPL in the runtime, never
   * called; if that policy ever changes, its unpacker registration goes
   * here. */
}

LOAD_LIB_INIT(libvkd3d_d3d12, OnInit)
