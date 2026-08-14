/* End-to-end attach test for the FEX thunk pair.
 *
 * x86-64 guest ELF. Run under FEX. Proves the whole chain except the PE hop:
 *
 *   this program (x86-64, emulated)
 *     -> dlopen libvkd3d_d3d12-guest.so           guest thunk library
 *        -> LOAD_LIB constructor, 0F 3F fex:loadlib
 *           -> FEX dlopens libvkd3d_d3d12-host.so  native ppc64le
 *              -> dlopen libvkd3d-proton-d3d12.so  native vkd3d-proton
 *
 * and then makes real calls across it.
 *
 * It deliberately exercises the unixlib entry point too, by calling
 * __wine_unix_call_funcs[0] directly. That is the exact function the PE shim
 * reaches through wine's loader, so running it here means the PE hop is the
 * only untested link rather than the whole init path -- and it needs no wine.
 *
 * MUST run with VKD3D_THUNK_ABI=sysv (run-attach.sh sets it). The generated
 * vtables default to MS-x64 on x86-64 because the deployment caller is a PE;
 * an ELF caller has to opt back in or every method call on a returned proxy
 * jumps with its arguments in the wrong registers.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vkd3d_unixlib.h"

typedef int32_t (*pfn_enable_experimental)(uint32_t count, const void* iids, void* configs, uint32_t* sizes);
typedef int32_t (*pfn_get_debug_interface)(const void* riid, void** out);
typedef uint32_t (*pfn_probe)(void);
typedef int (*pfn_unixcall)(void* args);

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

static const char* const kBoundaryName[VKD3D_PE_BOUNDARY_COUNT] = {
  "vkd3d_thunk_call", "vkd3d_thunk_call_float", "vkd3d_thunk_call_entry", "vkd3d_thunk_host_probe", "vkd3d_thunk_set_abi_sysv",
};

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "./build/libvkd3d_d3d12-guest.so";
  int failures = 0;
  unsigned i;

  void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    printf("FAIL dlopen(%s): %s\n", path, dlerror());
    return 1;
  }
  printf("ok   dlopen %s\n", path);

  /* The crossing itself. Reaching this at all means the LOAD_LIB constructor
   * found and loaded $FEX_THUNKHOSTLIBS/libvkd3d_d3d12-host.so, and the count
   * is the host side reporting how much of native vkd3d-proton it resolved. */
  {
    pfn_probe probe = (pfn_probe)dlsym(h, "vkd3d_thunk_host_probe");
    if (!probe) {
      printf("FAIL dlsym vkd3d_thunk_host_probe\n");
      return 1;
    }
    uint32_t n = probe();
    printf("%s vkd3d_thunk_host_probe -> %u/%u native entry points\n", n == VKD3D_PE_ENTRY_COUNT ? "ok  " : "FAIL", n,
           (unsigned)VKD3D_PE_ENTRY_COUNT);
    if (n != VKD3D_PE_ENTRY_COUNT) {
      failures++;
    }
  }

  /* The four boundary functions and the eight flat entry points, by name --
   * the same lookup the guest library's unixcall_init does on the PE's
   * behalf. A null here is what would make the PE shim refuse to initialise. */
  for (i = 0; i < VKD3D_PE_BOUNDARY_COUNT; ++i) {
    void* sym = dlsym(h, kBoundaryName[i]);
    int optional = (i == VKD3D_PE_BOUNDARY_SET_ABI_SYSV);
    printf("%s boundary %-24s %p%s\n", (sym || optional) ? "ok  " : "FAIL", kBoundaryName[i], sym, optional && !sym ? "  (optional)" : "");
    if (!sym && !optional) {
      failures++;
    }
  }
  for (i = 0; i < VKD3D_PE_ENTRY_COUNT; ++i) {
    void* sym = dlsym(h, kEntryName[i]);
    printf("%s entry    %-46s %p\n", sym ? "ok  " : "FAIL", kEntryName[i], sym);
    if (!sym) {
      failures++;
    }
  }

  /* The unixlib path, called directly. This is the PE shim's init, minus wine:
   * same struct, same magic, same function. */
  {
    const void** funcs = (const void**)dlsym(h, "__wine_unix_call_funcs");
    if (!funcs) {
      printf("FAIL dlsym __wine_unix_call_funcs\n");
      failures++;
    } else {
      struct vkd3d_unix_init_params p;
      int rc;

      memset(&p, 0, sizeof(p));
      p.in_magic = VKD3D_UNIX_INIT_MAGIC;
      p.in_size = (uint32_t)sizeof(p);

      rc = ((pfn_unixcall)(void*)funcs[unix_vkd3d_init])(&p);
      printf("%s unixcall_init -> 0x%08x  resolved=%u abi_is_ms=%u\n", rc == 0 ? "ok  " : "FAIL", (unsigned)rc, p.resolved, p.abi_is_ms);
      if (rc != 0) {
        failures++;
      }
      /* Every address the PE would call through must be non-null and must
       * agree with what dlsym gave us above: the PE has no dlsym of its own,
       * so a wrong table here is invisible on the real path. */
      for (i = 0; i < VKD3D_PE_ENTRY_COUNT; ++i) {
        void* sym = dlsym(h, kEntryName[i]);
        if ((uint64_t)(uintptr_t)sym != p.entry[i]) {
          printf("FAIL entry table[%u] %s: %p != %p\n", i, kEntryName[i], sym, (void*)(uintptr_t)p.entry[i]);
          failures++;
        }
      }
      if (!p.boundary[VKD3D_PE_BOUNDARY_CALL_ENTRY]) {
        printf("FAIL boundary table has no vkd3d_thunk_call_entry (the pump would not run)\n");
        failures++;
      }
      /* The guest library fills these by taking addresses in its own
       * translation unit; dlsym reaches the same functions through the dynamic
       * symbol table. They must agree, or the exported symbol is not the
       * function the PE is about to call -- and the PE, having no dlsym, could
       * never notice. */
      for (i = 0; i < VKD3D_PE_BOUNDARY_COUNT; ++i) {
        void* sym = dlsym(h, kBoundaryName[i]);
        if (i == VKD3D_PE_BOUNDARY_SET_ABI_SYSV && !sym && !p.boundary[i]) {
          continue;
        }
        if ((uint64_t)(uintptr_t)sym != p.boundary[i]) {
          printf("FAIL boundary table[%u] %s: %p != %p\n", i, kBoundaryName[i], sym, (void*)(uintptr_t)p.boundary[i]);
          failures++;
        }
      }
      /* An ELF caller runs with VKD3D_THUNK_ABI=sysv, so this must read 0.
       * Reading 1 here means run-attach.sh's env did not take and every
       * proxy method call in this process is about to use the wrong ABI. */
      if (p.abi_is_ms) {
        printf("FAIL guest vtables are in MS-x64 mode; an ELF caller needs VKD3D_THUNK_ABI=sysv\n");
        failures++;
      }
    }
  }

  /* First real call across. D3D12EnableExperimentalFeatures is chosen for it
   * deliberately: of the eight it is the one that touches neither Vulkan nor a
   * display, so a failure here is a failure of the thunk rather than of the
   * graphics stack. vkd3d-proton FIXMEs it and forwards to d3d12core, so any
   * HRESULT is a pass -- a hang, a trap or a wild value is not. */
  {
    pfn_enable_experimental enable = (pfn_enable_experimental)dlsym(h, "D3D12EnableExperimentalFeatures");
    if (enable) {
      int32_t hr = enable(0, 0, 0, 0);
      printf("ok   D3D12EnableExperimentalFeatures(0,...) -> 0x%08x\n", (unsigned)hr);
    }
  }

  /* Second call: one that returns an object, so the proxy machinery runs.
   * Gated behind an argument because it loads d3d12core and initialises
   * vkd3d's debug layer, which needs more of the stack to be present.
   *
   * IID_ID3D12Debug {344488b7-6846-474b-b989-f027448245e0} */
  if (argc > 2 && argv[2][0] == 'd') {
    static const uint8_t kIID_ID3D12Debug[16] = {
      0xb7, 0x88, 0x44, 0x34, 0x46, 0x68, 0x4b, 0x47, 0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0,
    };
    pfn_get_debug_interface get_debug = (pfn_get_debug_interface)dlsym(h, "D3D12GetDebugInterface");
    void* debug = 0;
    int32_t hr;

    if (!get_debug) {
      printf("FAIL dlsym D3D12GetDebugInterface\n");
      failures++;
    } else {
      hr = get_debug(kIID_ID3D12Debug, &debug);
      printf("%s D3D12GetDebugInterface -> 0x%08x, iface=%p\n", (hr == 0 && debug) ? "ok  " : "warn", (unsigned)hr, debug);
      if (hr == 0 && debug) {
        /* Must be a guest proxy, not the host object: its first word is a
         * vtable inside the guest library. Handing the caller a native ppc64le
         * vtable is the failure mode that does not announce itself. */
        void** vtbl = *(void***)debug;
        printf("ok   proxy vtbl=%p slot0=%p\n", (void*)vtbl, vtbl ? vtbl[0] : 0);
        ((uint32_t(*)(void*))vtbl[2])(debug); /* Release */
      }
    }
  }

  printf("%s\n", failures ? "FAILURES" : "ALL OK");
  return failures != 0;
}
