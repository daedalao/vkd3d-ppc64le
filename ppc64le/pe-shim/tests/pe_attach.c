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
