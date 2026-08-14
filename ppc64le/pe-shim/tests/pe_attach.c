/* pe_attach.exe -- the PE half of the attach test.
 *
 * x86-64 PE, built with the same freestanding recipe as the shim itself (no
 * CRT, no mingw sysroot, own entry point). Run it under Wine, under FEX, on
 * the POWER box, from a directory containing d3d12.dll:
 *
 *   wine pe_attach.exe
 *
 * It is the only thing that can exercise the one link dxvk-ppc64le never
 * closed: PE -> wine unixlib -> guest ELF. Everything below that seam is
 * already covered by tests/attach.c, which needs no wine at all.
 *
 * What it checks, in order:
 *   1. d3d12.dll loads at all (as a PE, by the normal loader).
 *   2. all ten exports resolve, including the two DATA ones -- a DATA export
 *      that came out as a function would make D3D12SDKVersion read as code.
 *   3. D3D12SDKVersion really reads 619 through the export table.
 *   4. D3D12EnableExperimentalFeatures(0, NULL, NULL, NULL) crosses. It is
 *      chosen because it is the only one of the eight that touches neither
 *      Vulkan nor a display: a failure is a failure of the boundary, not of
 *      the graphics stack.
 *   5. the shim's own diagnostics, which say WHICH unixlib route wine
 *      offered (1002 by-name, or the 1000 builtin fallback) and how much of
 *      native vkd3d-proton the host side found. That answers the open
 *      question dxvk-ppc64le recorded as gap #2 -- whether this wine has
 *      MemoryWineLoadUnixLibByName -- with one number.
 */

#include <stdint.h>

#define WINAPI __attribute__((ms_abi))
#define IMP __declspec(dllimport)

typedef void* HANDLE;
typedef void* HMODULE;

IMP HMODULE WINAPI LoadLibraryA(const char* name);
IMP int32_t WINAPI FreeLibrary(HMODULE module);
IMP void* WINAPI GetProcAddress(HMODULE module, const char* name);
IMP uint32_t WINAPI GetLastError(void);
IMP HANDLE WINAPI GetStdHandle(uint32_t which);
IMP int32_t WINAPI WriteFile(HANDLE file, const void* buf, uint32_t len, uint32_t* written, void* overlapped);
IMP void WINAPI ExitProcess(uint32_t code);
IMP HANDLE WINAPI CreateEventA(void* sa, int32_t manual, int32_t initial, const char* name);
IMP uint32_t WINAPI WaitForSingleObject(HANDLE h, uint32_t millis);
IMP int32_t WINAPI CloseHandle(HANDLE h);

#define STD_OUTPUT_HANDLE ((uint32_t) - 11)

/* --- output, without a CRT ------------------------------------------------ */

static HANDLE g_Out;

static unsigned SLen(const char* s) {
  unsigned n = 0;
  while (s[n]) {
    ++n;
  }
  return n;
}

static void Put(const char* s) {
  uint32_t written = 0;
  if (!g_Out) {
    g_Out = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  WriteFile(g_Out, s, SLen(s), &written, 0);
}

static void PutHex(uint64_t v, unsigned digits) {
  static const char kHex[] = "0123456789abcdef";
  char buf[19];
  unsigned i;

  buf[0] = '0';
  buf[1] = 'x';
  for (i = 0; i < digits; ++i) {
    buf[2 + i] = kHex[(v >> (4 * (digits - 1 - i))) & 0xf];
  }
  buf[2 + digits] = 0;
  Put(buf);
}

static void PutDec(uint64_t v) {
  char buf[21];
  unsigned n = 0;
  unsigned i;

  if (!v) {
    Put("0");
    return;
  }
  while (v) {
    buf[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  for (i = 0; i < n / 2; ++i) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  buf[n] = 0;
  Put(buf);
}

/* --- the shim's surface --------------------------------------------------- */

typedef int32_t(WINAPI* pfn_enable_experimental)(uint32_t count, const void* iids, void* configs, uint32_t* sizes);

static const char* const kExport[] = {
  "D3D12CreateDevice",
  "D3D12GetDebugInterface",
  "D3D12GetInterface",
  "D3D12CreateRootSignatureDeserializer",
  "D3D12CreateVersionedRootSignatureDeserializer",
  "D3D12SerializeRootSignature",
  "D3D12SerializeVersionedRootSignature",
  "D3D12EnableExperimentalFeatures",
  "D3D12SDKVersion",
  "D3D12SDKPath",
};
#define EXPORT_COUNT (sizeof(kExport) / sizeof(kExport[0]))

/* Diagnostics the shim exports so a PE test can see under the boundary. A PE
 * has no dlsym, so without these nothing about the crossing is observable from
 * here. */
static const char* const kDiag[] = {
  "vkd3d_shim_unixlib_route", "vkd3d_shim_unixlib_status", "vkd3d_shim_resolved",
  "vkd3d_shim_abi_is_ms",     "vkd3d_shim_pump_state",     "vkd3d_shim_pump_events",
};
#define DIAG_COUNT (sizeof(kDiag) / sizeof(kDiag[0]))

int32_t WINAPI mainCRTStartup(void) {
  HMODULE mod;
  unsigned failures = 0;
  unsigned i;

  Put("pe_attach: LoadLibraryA(\"d3d12.dll\")\n");
  mod = LoadLibraryA(".\\d3d12.dll");
  if (!mod) {
    mod = LoadLibraryA("d3d12.dll");
  }
  if (!mod) {
    Put("FAIL LoadLibraryA, GetLastError=");
    PutDec(GetLastError());
    Put("\n");
    ExitProcess(1);
  }
  Put("ok   d3d12.dll loaded at ");
  PutHex((uint64_t)(uintptr_t)mod, 16);
  Put("\n");

  for (i = 0; i < EXPORT_COUNT; ++i) {
    void* p = GetProcAddress(mod, kExport[i]);
    Put(p ? "ok   " : "FAIL ");
    Put(kExport[i]);
    Put(" -> ");
    PutHex((uint64_t)(uintptr_t)p, 16);
    Put("\n");
    if (!p) {
      ++failures;
    }
  }

  /* DATA exports read as data. GetProcAddress on a DATA export yields the
   * address of the variable, not of a stub, so this dereference is the check
   * that the export table really marked them as data. */
  {
    const uint32_t* sdk_version = (const uint32_t*)GetProcAddress(mod, "D3D12SDKVersion");
    const char* const* sdk_path = (const char* const*)GetProcAddress(mod, "D3D12SDKPath");
    if (sdk_version) {
      Put("ok   D3D12SDKVersion = ");
      PutDec(*sdk_version);
      Put(*sdk_version == 619 ? "  (matches include/vkd3d_d3d12.idl)\n" : "  UNEXPECTED, want 619\n");
      if (*sdk_version != 619) {
        ++failures;
      }
    }
    if (sdk_path && *sdk_path) {
      Put("ok   D3D12SDKPath = \"");
      Put(*sdk_path);
      Put("\"\n");
    }
  }

  /* The crossing. Everything before this point is loader mechanics; this is
   * the first byte that reaches the guest ELF library, and through it native
   * ppc64le vkd3d-proton. */
  {
    pfn_enable_experimental enable = (pfn_enable_experimental)GetProcAddress(mod, "D3D12EnableExperimentalFeatures");
    if (!enable) {
      Put("FAIL no D3D12EnableExperimentalFeatures\n");
      ++failures;
    } else {
      int32_t hr = enable(0, 0, 0, 0);
      Put("ok   D3D12EnableExperimentalFeatures(0,...) -> ");
      PutHex((uint32_t)hr, 8);
      Put("\n");
      /* 0x80004005 (E_FAIL) is this shim's own "the boundary did not come up"
       * value, and it is the one HRESULT that means the test failed rather
       * than that vkd3d declined the request. */
      if (hr == (int32_t)0x80004005) {
        Put("FAIL E_FAIL is the shim's boundary-failure value; see the diagnostics below\n");
        ++failures;
      }
    }
  }

  /* --- the device: MS-ABI proxy vtables against real native vkd3d ---------
   *
   * This is the exact call shape a game emits: MSVC-convention indirect calls
   * through the proxy vtable (the generated ms_abi forwarders), against a
   * real device on the real GPU. Everything the SysV gpu_roundtrip proved is
   * re-proven here in the deployment ABI, plus the one thing only a PE can
   * prove: the fence pump signalling a REAL Win32 event that
   * WaitForSingleObject wakes on. */
  {
    typedef int32_t(WINAPI* pfn_create_device)(void* adapter, uint32_t fl, const void* riid, void** out);
    /* generated ms_abi worker shapes: (proxy, uint64 args...) */
    typedef uint32_t(WINAPI* mfn_release)(void*);
    typedef uint64_t(WINAPI* mfn_u0)(void*);
    typedef uint64_t(WINAPI* mfn_u1)(void*, uint64_t);
    typedef uint64_t(WINAPI* mfn_u2)(void*, uint64_t, uint64_t);
    typedef uint64_t(WINAPI* mfn_u3)(void*, uint64_t, uint64_t, uint64_t);
    typedef uint64_t(WINAPI* mfn_u4)(void*, uint64_t, uint64_t, uint64_t, uint64_t);
    /* aggregate return, MSVC convention: (this, &out) -> &out */
    typedef void*(WINAPI* mfn_agg)(void*, void*);

    static const uint8_t kIID_Device[16] = {0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
                                            0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7};
    static const uint8_t kIID_Fence[16] = {0xcf, 0x3d, 0x75, 0x0a, 0xd8, 0xc4, 0x91, 0x4b,
                                           0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76};
    static const uint8_t kIID_DescHeap[16] = {0x1d, 0x47, 0xfb, 0x8e, 0x6c, 0x61, 0x49, 0x4f,
                                              0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51};
    struct {
      int32_t Type;
      uint32_t NumDescriptors;
      uint32_t Flags;
      uint32_t NodeMask;
    } heap_desc = {0 /*CBV_SRV_UAV*/, 64, 0, 0};

    pfn_create_device create = (pfn_create_device)GetProcAddress(mod, "D3D12CreateDevice");
    void* dev = 0;
    int32_t hr = create ? create(0, 0xc000 /*FL 12_0*/, kIID_Device, &dev) : (int32_t)0x80004005;
    Put((hr == 0 && dev) ? "ok   " : "FAIL ");
    Put("D3D12CreateDevice (MS ABI) -> ");
    PutHex((uint32_t)hr, 8);
    Put("\n");
    if (hr != 0 || !dev) {
      ++failures;
    } else {
      void** dv = *(void***)dev;

      /* fence + CPU signal + completed value, all through ms_abi forwarders */
      void* fence = 0;
      hr = (int32_t)((mfn_u4)dv[36])(dev, 0, 0, (uint64_t)(uintptr_t)kIID_Fence, (uint64_t)(uintptr_t)&fence);
      Put((hr == 0 && fence) ? "ok   CreateFence\n" : "FAIL CreateFence\n");
      if (hr != 0 || !fence) {
        ++failures;
      } else {
        void** fv = *(void***)fence;

        ((mfn_u1)fv[10])(fence, 1); /* Signal(1), CPU side */
        uint64_t completed = ((mfn_u0)fv[8])(fence);
        Put(completed == 1 ? "ok   GetCompletedValue == 1\n" : "FAIL GetCompletedValue != 1\n");
        if (completed != 1) {
          ++failures;
        }

        /* THE PUMP, with a real Win32 event: register for value 2, signal,
         * and wait on the event. The chain behind this wait: native vkd3d
         * worker -> eventfd -> host reaper -> doorbell -> the shim's pump
         * thread -> NtSetEvent -> this WaitForSingleObject. */
        HANDLE ev = CreateEventA(0, 0, 0, 0);
        hr = (int32_t)((mfn_u2)fv[9])(fence, 2, (uint64_t)(uintptr_t)ev);
        Put(hr == 0 ? "ok   SetEventOnCompletion(2, hEvent)\n" : "FAIL SetEventOnCompletion\n");
        ((mfn_u1)fv[10])(fence, 2); /* Signal(2) */
        {
          uint32_t w = WaitForSingleObject(ev, 5000);
          Put(w == 0 ? "ok   WaitForSingleObject(hEvent) -> signalled by the pump\n"
                     : "FAIL WaitForSingleObject on the pump event\n");
          if (w != 0) {
            ++failures;
          }
        }
        CloseHandle(ev);
        ((mfn_release)fv[2])(fence);
      }

      /* aggregate return under the MSVC convention against real vkd3d:
       * GetCPUDescriptorHandleForHeapStart is (this, &out) -> &out. */
      {
        void* heap = 0;
        hr = (int32_t)((mfn_u3)dv[14])(dev, (uint64_t)(uintptr_t)&heap_desc, (uint64_t)(uintptr_t)kIID_DescHeap,
                                       (uint64_t)(uintptr_t)&heap);
        Put((hr == 0 && heap) ? "ok   CreateDescriptorHeap\n" : "FAIL CreateDescriptorHeap\n");
        if (hr == 0 && heap) {
          void** hv = *(void***)heap;
          uint64_t out = 0;
          void* ret = ((mfn_agg)hv[9])(heap, &out);
          Put((ret == (void*)&out && out) ? "ok   GetCPUDescriptorHandleForHeapStart (MSVC aggregate return)\n"
                                          : "FAIL aggregate return: ret != &out or handle is 0\n");
          if (ret != (void*)&out || !out) {
            ++failures;
          }
          ((mfn_release)hv[2])(heap);
        } else {
          ++failures;
        }
      }

      ((mfn_release)dv[2])(dev);
    }
  }

  Put("--- shim diagnostics\n");
  for (i = 0; i < DIAG_COUNT; ++i) {
    const uint64_t* p = (const uint64_t*)GetProcAddress(mod, kDiag[i]);
    Put("     ");
    Put(kDiag[i]);
    Put(" = ");
    if (!p) {
      Put("<absent>\n");
      continue;
    }
    /* pump_events is the only 64-bit one; the rest are 32-bit. Reading a
     * 32-bit variable as 32 bits matters -- the next variable is not
     * guaranteed to follow it. */
    if (i == DIAG_COUNT - 1) {
      PutDec(*p);
    } else {
      PutDec(*(const uint32_t*)p);
    }
    Put("\n");
  }
  Put("     (route 1002 = this wine has MemoryWineLoadUnixLibByName; 1000 = it\n"
      "      does not and the builtin fallback was tried; 0 = init never got as\n"
      "      far as the unixlib call. resolved wants 8.)\n");

  FreeLibrary(mod);

  Put(failures ? "FAILURES\n" : "ALL OK\n");
  ExitProcess(failures ? 1 : 0);
  return 0;
}
