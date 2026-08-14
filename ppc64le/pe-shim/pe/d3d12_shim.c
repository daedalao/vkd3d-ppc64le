/* d3d12.dll -- x86-64 PE shim.
 *
 * The only reason this file exists: Wine loads d3d12.dll as a PE image, and
 * FEX's thunk mechanism substitutes guest ELF .so files at dlopen. There is no
 * ELF seam under d3d12.dll for ThunksDB to overlay. This provides one, by
 * being a PE that does nothing except reach the guest ELF library.
 *
 * It exports the eight flat entry points (libs/d3d12/d3d12.def) plus the two
 * AgilitySDK data symbols. On the first call it loads
 * libvkd3d_d3d12-guest.so through Wine's unixlib mechanism, receives the guest
 * ELF addresses of the same eight functions, and thereafter each export is a
 * one-line forward through a function pointer. It never sees a COM object: the
 * proxies handed back carry vtables that live in the guest ELF library, so
 * every subsequent method call goes game -> ELF stub -> 0F 3F -> native
 * vkd3d-proton without touching this code at all.
 *
 * The one thing it owns beyond forwarding is the FENCE PUMP THREAD -- see
 * "The fence pump" below and ppc64le/thunk/runtime/vkd3d_thunk_abi.h.
 *
 * -- Why there is no windows.h here ---------------------------------------
 *
 * The build machine has no mingw-w64 sysroot in the configuration this was
 * developed against, so <windows.h>, <winternl.h> and the import libraries
 * that come with them are unavailable. clang still emits x86-64 COFF and lld
 * still links it, so the handful of NT declarations needed are written out
 * below and the ntdll import library is synthesised with llvm-dlltool from
 * pe/ntdll.def. If a sysroot appears later, this block can be replaced by
 * #include <winternl.h> and nothing else changes. Everything this file needs
 * beyond the boundary -- thread creation, event signalling -- is taken from
 * ntdll rather than kernel32, which is why the import list stays one DLL.
 */

#include <stdint.h>

#include "vkd3d_unixlib.h"

/* --- Minimal NT surface --------------------------------------------------- */

typedef int32_t NTSTATUS;
typedef uint16_t WCHAR;

typedef struct {
  uint16_t Length;
  uint16_t MaximumLength;
  WCHAR* Buffer;
} UNICODE_STRING;

typedef struct {
  void* UniqueProcess;
  void* UniqueThread;
} CLIENT_ID;

/* include/winternl.h: MemoryWineLoadUnixLib = 1000, then
 * MemoryWineLoadUnixLibWow64, then ByName.
 *
 * ByName (1002) does NOT exist in every wine: dxvk-ppc64le measured
 * GE-Proton11-3's wine-11.0 returning STATUS_INVALID_INFO_CLASS for
 * 1002/1003/1004 while answering 1000 and 1001, so the by-name loader
 * postdates it. The fallback is the older MemoryWineLoadUnixLib (1000), which
 * takes this DLL's own module base and loads the .so that wine associated with
 * it when it loaded this DLL as a BUILTIN -- i.e. it requires a d3d12.dll +
 * d3d12.so pair in a WINEDLLPATH directory and d3d12=b. Both routes are wired
 * here; which one ran is readable from the diagnostic exports below. */
#define MemoryWineLoadUnixLib 1000
#define MemoryWineLoadUnixLibByName 1002

#define STATUS_SUCCESS ((NTSTATUS)0)
/* 0xC0000003 is STATUS_INVALID_INFO_CLASS; wine's NtQueryVirtualMemory returns
 * STATUS_INVALID_PARAMETER (0xC000000D) for an unknown class on some builds,
 * so both are treated as "this wine has no by-name loader". */
#define STATUS_INVALID_INFO_CLASS ((NTSTATUS)0xC0000003)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)

/* lld defines this for PE targets; it is the image base of this DLL. */
extern char __ImageBase[];

#define NTAPI __attribute__((ms_abi))
#define NTIMP __declspec(dllimport)
#define GUESTABI __attribute__((sysv_abi))

NTIMP NTSTATUS NTAPI NtQueryVirtualMemory(void* process, const void* addr, int info_class, void* buffer, uint64_t len, uint64_t* res_len);
NTIMP NTSTATUS NTAPI LdrGetDllHandle(const WCHAR* path, uint32_t flags, const UNICODE_STRING* name, void** base);
NTIMP void* NTAPI RtlFindExportedRoutineByName(void* module, const char* name);
NTIMP void NTAPI RtlInitUnicodeString(UNICODE_STRING* target, const WCHAR* src);
NTIMP NTSTATUS NTAPI RtlQueryEnvironmentVariable_U(void* env, UNICODE_STRING* name, UNICODE_STRING* value);

/* The pump thread's two ntdll needs. kernel32!CreateThread forwards to
 * RtlCreateUserThread and kernel32!SetEvent forwards to NtSetEvent, so nothing
 * is lost by taking them from ntdll directly -- and a great deal is gained: no
 * second import library, no LdrGetDllHandle/RtlFindExportedRoutineByName dance
 * that can fail at run time, and a missing symbol becomes a load-time failure
 * with a name attached instead of a silent no-pump. */
NTIMP NTSTATUS NTAPI RtlCreateUserThread(void* process, void* descr, uint8_t suspended, uint32_t zero_bits, uint64_t stack_reserve,
                                         uint64_t stack_commit, void* start, void* param, void** handle, CLIENT_ID* id);
NTIMP NTSTATUS NTAPI NtSetEvent(void* handle, int32_t* prev_state);

typedef NTSTATUS(NTAPI* unix_call_fn)(uint64_t handle, unsigned int code, void* args);

#define S_OK ((int32_t)0)
#define E_FAIL ((int32_t)0x80004005)
#define E_NOINTERFACE ((int32_t)0x80004002)

/* --- The eight entry points, as the guest ELF library defines them --------- */
/* Signatures mirror libs/d3d12/main.c exactly. Spelled with plain integer and
 * void* types rather than the D3D12 typedefs for the same reason everything
 * else in this file is: there is no Windows SDK on either side of this build.
 * Every parameter is integer-class, so the ABI is unambiguous without the real
 * types.
 *
 * GUESTABI is the whole point of this block. This translation unit is compiled
 * for the Microsoft x64 ABI (first four integer arguments in rcx/rdx/r8/r9);
 * the guest ELF library is compiled for System V (rdi/rsi/rdx/rcx/r8/r9).
 * Those disagree from the first argument onwards, so calling across without
 * saying so would hand vkd3d-proton the arguments in the wrong registers --
 * and silently, because the values are all plausible integers. sysv_abi on the
 * function-pointer type makes clang emit the shuffle at each call site, which
 * is why nothing has to change on the ELF side.
 *
 * The inbound direction needs no such marking: the exports below are ms_abi,
 * which is what a Windows game calls them with. Same for the generated COM
 * vtables, which default to MS-x64 on x86-64 precisely because the deployment
 * caller is this PE (vkd3d_thunk_abi.h, "ABI mode"). */

typedef int32_t(GUESTABI* pfn_D3D12CreateDevice)(void* pAdapter, uint32_t MinimumFeatureLevel, const void* riid, void** ppDevice);
typedef int32_t(GUESTABI* pfn_D3D12GetDebugInterface)(const void* riid, void** ppvDebug);
typedef int32_t(GUESTABI* pfn_D3D12GetInterface)(const void* rclsid, const void* riid, void** ppvDebug);
/* Shared by both deserializer entry points: identical signatures, different
 * native implementations. */
typedef int32_t(GUESTABI* pfn_D3D12CreateRootSignatureDeserializer)(const void* pSrcData, uint64_t SrcDataSizeInBytes, const void* pRootSignatureDeserializerInterface,
                                                                    void** ppRootSignatureDeserializer);
typedef int32_t(GUESTABI* pfn_D3D12SerializeRootSignature)(const void* pRootSignature, uint32_t Version, void** ppBlob, void** ppErrorBlob);
typedef int32_t(GUESTABI* pfn_D3D12SerializeVersionedRootSignature)(const void* pRootSignature, void** ppBlob, void** ppErrorBlob);
typedef int32_t(GUESTABI* pfn_D3D12EnableExperimentalFeatures)(uint32_t NumFeatures, const void* pIIDs, void* pConfigurationStructs,
                                                               uint32_t* pConfigurationStructSizes);

/* The boundary. Only call_entry is used from here (the pump). */
typedef uint32_t(GUESTABI* pfn_thunk_call_entry)(uint32_t entry, uint64_t* args);

/* Entry ids for vkd3d_thunk_call_entry. Duplicated from
 * ppc64le/thunk/runtime/vkd3d_thunk_abi.h rather than included, because that
 * header is the ELF side's and this translation unit has a different target;
 * the two values are frozen protocol constants, not implementation detail. */
#define VKD3D_ENTRY_PUMP_WAIT 32u
#define VKD3D_ENTRY_PUMP_SHUTDOWN 33u

/* --- Diagnostics ---------------------------------------------------------- */
/* A PE has no dlsym, so a test cannot otherwise see anything that happens
 * under the boundary. These are exported so tests/pe_attach.c can read them
 * through GetProcAddress. Nothing in the call path depends on them and they
 * are not part of the d3d12 ABI. */
__declspec(dllexport) int32_t vkd3d_shim_unixlib_route = 0;   /* 1002, 1000, or 0 */
__declspec(dllexport) int32_t vkd3d_shim_unixlib_status = -1; /* the NTSTATUS that decided it */
__declspec(dllexport) uint32_t vkd3d_shim_resolved = 0;       /* host-side entry points found (want 8) */
__declspec(dllexport) uint32_t vkd3d_shim_abi_is_ms = 0;      /* guest vtable ABI mode */
__declspec(dllexport) int32_t vkd3d_shim_pump_state = 0;      /* 0 not started, 1 running, -1 failed */
__declspec(dllexport) uint64_t vkd3d_shim_pump_events = 0;    /* events signalled so far */

/* --- AgilitySDK data exports ---------------------------------------------- */
/* d3d12core.dll exports D3D12SDKVersion as data (libs/d3d12core/d3d12core.def,
 * "D3D12SDKVersion DATA PRIVATE"); this shim carries both symbols because
 * docs/d3d12-boundary-analysis.md §1 lists them as part of what d3d12.dll
 * presents to AgilitySDK-aware callers.
 *
 * 619 is not a guess: include/vkd3d_d3d12.idl:381 declares
 * `const UINT D3D12_SDK_VERSION = 619` and ppc64le/idl/gen/vkd3d_d3d12.h:1255
 * carries the same value into the generated headers. It is deliberately taken
 * from THIS tree rather than from the AgilitySDK, so the number a caller reads
 * out of the shim is the number the vkd3d-proton underneath it implements.
 *
 * D3D12SDKPath is a `const char*` to a UTF-8 string, NOT a WCHAR string: that
 * is the documented AgilitySDK contract and it is what this repository's own
 * tests/d3d12.c:37 exports (`const char *D3D12SDKPath = u8".\\D3D12\\"`).
 * Spelled here as a plain char array + pointer because there is no CRT to
 * place a string literal for us. */
__declspec(dllexport) const uint32_t D3D12SDKVersion = 619;
static const char kSDKPath[] = ".\\D3D12\\";
__declspec(dllexport) const char* const D3D12SDKPath = kSDKPath;

/* --- Locating the guest ELF library --------------------------------------- */

static const WCHAR kDefaultSo[] = u"\\??\\unix/usr/local/lib/fex-emu/libvkd3d_d3d12-guest.so";
static const WCHAR kEnvName[] = u"VKD3D_THUNK_GUEST_SO";
static const WCHAR kUnixPrefix[] = u"\\??\\unix";

static WCHAR g_PathBuf[600];
static unix_call_fn g_Dispatch;
static uint64_t g_UnixHandle;
static struct vkd3d_unix_init_params g_Init;
static int32_t g_InitState; /* 0 untried, 1 ready, -1 failed */

static void* CurrentProcess(void) {
  return (void*)(intptr_t)-1;
}

static unsigned WLen(const WCHAR* s) {
  unsigned n = 0;
  while (s[n]) {
    ++n;
  }
  return n;
}

/* VKD3D_THUNK_GUEST_SO overrides the location. A value starting with '/' is
 * taken as a Unix path and gets wine's \??\unix prefix; anything else is
 * passed through, so an NT path or a bare name (searched in wine's dll
 * directories, dlls/ntdll/unix/loader.c) both work. */
static const WCHAR* ResolveSoPath(void) {
  UNICODE_STRING name;
  UNICODE_STRING value;
  unsigned prefix_len = WLen(kUnixPrefix);
  unsigned i;

  RtlInitUnicodeString(&name, kEnvName);

  /* Leave room at the front for the prefix so it can be prepended in place. */
  value.Buffer = &g_PathBuf[prefix_len];
  value.Length = 0;
  value.MaximumLength = (uint16_t)((sizeof(g_PathBuf) / sizeof(WCHAR) - prefix_len - 1) * sizeof(WCHAR));

  if (RtlQueryEnvironmentVariable_U(0, &name, &value) != STATUS_SUCCESS || value.Length == 0) {
    return kDefaultSo;
  }
  g_PathBuf[prefix_len + value.Length / sizeof(WCHAR)] = 0;

  if (g_PathBuf[prefix_len] == u'/') {
    for (i = 0; i < prefix_len; ++i) {
      g_PathBuf[i] = kUnixPrefix[i];
    }
    return g_PathBuf;
  }
  return &g_PathBuf[prefix_len];
}

static int EnsureLoaded(void) {
  static const WCHAR ntdll_name[] = u"ntdll.dll";
  UNICODE_STRING ntdll;
  UNICODE_STRING so_name;
  void* ntdll_module = 0;
  void** dispatch_slot;
  uint64_t res[2];
  NTSTATUS status;
  unsigned i;
  int32_t state = __atomic_load_n(&g_InitState, __ATOMIC_ACQUIRE);

  if (state) {
    return state > 0;
  }

  /* __wine_unix_call_dispatcher is a DATA export from ntdll holding the
   * function pointer. This is what libs/winecrt0/unix_lib.c does; reproduced
   * here because winecrt0 is a PE static library we do not have. */
  RtlInitUnicodeString(&ntdll, ntdll_name);
  if (LdrGetDllHandle(0, 0, &ntdll, &ntdll_module) != STATUS_SUCCESS || !ntdll_module) {
    goto fail;
  }
  dispatch_slot = (void**)RtlFindExportedRoutineByName(ntdll_module, "__wine_unix_call_dispatcher");
  if (!dispatch_slot || !*dispatch_slot) {
    goto fail;
  }
  g_Dispatch = (unix_call_fn)*dispatch_slot;

  /* Equivalent to wine's __wine_load_unix_lib(): dlopen the guest ELF library
   * and dlsym its __wine_unix_call_funcs. res[0] is the dlopen handle (kept
   * only so an unload path could be added later), res[1] is the funcs table
   * that becomes our unixlib handle. */
  res[0] = 0;
  res[1] = 0;
  RtlInitUnicodeString(&so_name, ResolveSoPath());
  status = NtQueryVirtualMemory(CurrentProcess(), &so_name, MemoryWineLoadUnixLibByName, res, sizeof(res), 0);

  if (status == STATUS_INVALID_INFO_CLASS || status == STATUS_INVALID_PARAMETER) {
    /* This wine has no by-name unixlib loader. Fall back to the builtin one,
     * which needs no path because wine already recorded the .so next to this
     * DLL when it loaded it as a builtin. Fails cleanly if this DLL was loaded
     * as native, which is the honest outcome: there is then no way for a PE to
     * reach an arbitrary ELF on this wine. */
    uint64_t handle = 0;
    vkd3d_shim_unixlib_route = 1000;
    status = NtQueryVirtualMemory(CurrentProcess(), __ImageBase, MemoryWineLoadUnixLib, &handle, sizeof(handle), 0);
    if (status != STATUS_SUCCESS) {
      vkd3d_shim_unixlib_status = status;
      goto fail;
    }
    g_UnixHandle = handle;
  } else if (status != STATUS_SUCCESS) {
    vkd3d_shim_unixlib_status = status;
    goto fail;
  } else {
    vkd3d_shim_unixlib_route = 1002;
    g_UnixHandle = res[1];
  }
  vkd3d_shim_unixlib_status = STATUS_SUCCESS;

  g_Init.in_magic = VKD3D_UNIX_INIT_MAGIC;
  g_Init.in_size = (uint32_t)sizeof(g_Init);

  if (g_Dispatch(g_UnixHandle, unix_vkd3d_init, &g_Init) != STATUS_SUCCESS) {
    goto fail;
  }

  vkd3d_shim_resolved = g_Init.resolved;
  vkd3d_shim_abi_is_ms = g_Init.abi_is_ms;

  /* `resolved` is how many of vkd3d-proton's eight exports the HOST side
   * found. Anything short of eight means the native library is missing or is
   * not what it claims to be, and CreateDevice would otherwise fail later with
   * no explanation. */
  if (g_Init.resolved != VKD3D_PE_ENTRY_COUNT) {
    goto fail;
  }
  for (i = 0; i < VKD3D_PE_ENTRY_COUNT; ++i) {
    if (!g_Init.entry[i]) {
      goto fail;
    }
  }
  if (!g_Init.boundary[VKD3D_PE_BOUNDARY_CALL_ENTRY]) {
    goto fail;
  }

  __atomic_store_n(&g_InitState, 1, __ATOMIC_RELEASE);
  return 1;

fail:
  __atomic_store_n(&g_InitState, -1, __ATOMIC_RELEASE);
  return 0;
}

#define ENTRY(idx, type) ((type)(uintptr_t)g_Init.entry[idx])

/* --- The fence pump -------------------------------------------------------
 *
 * ID3D12Fence::SetEventOnCompletion(value, hEvent) hands vkd3d a guest Win32
 * HANDLE that vkd3d's own worker threads are supposed to signal. Those are
 * host pthreads; FEX forbids host->guest calls outside a guest-initiated
 * crossing, and native vkd3d would read the HANDLE bits as an eventfd
 * (include/private/vkd3d_native_sync_handle.h) -- which is doubly wrong on a
 * box running ntsync, where a Wine event has no fd at all.
 *
 * The resolution (vkd3d_thunk_abi.h, "fence/event pump",
 * docs/d3d12-boundary-analysis.md §4) puts a real eventfd on the host side and
 * a reaper thread behind it, and gives the guest ONE pump thread whose whole
 * job is to turn completions back into SetEvent calls. Every host->guest
 * transition is therefore the RETURN of a call the guest made, which is what
 * FEX permits.
 *
 * The thread lives here, in the PE, because SetEvent on a Wine HANDLE is
 * Win32 -- the guest ELF library cannot do it, and neither can native ppc64le
 * code. It is started at the first successful D3D12CreateDevice rather than at
 * DLL_PROCESS_ATTACH: creating a thread under loader lock is the wrong place
 * for it, and before a device exists there is nothing for it to wait on.
 *
 * The loop parks inside vkd3d_thunk_call_entry(PUMP_WAIT) -- a synchronous
 * crossing that blocks in native code on the host doorbell -- and comes back
 * either with a cookie to signal (ret == 1) or with a shutdown (ret == 0).
 * args[] is a stack array in this frame that the host writes through; legal
 * because the address space is shared and this thread is parked inside the
 * call for its whole duration.
 */

static void* g_PumpThread;
static int32_t g_PumpStarted; /* 0 untried, 1 started, -1 failed */

static uint32_t NTAPI PumpThreadProc(void* param) {
  pfn_thunk_call_entry call_entry = (pfn_thunk_call_entry)(uintptr_t)g_Init.boundary[VKD3D_PE_BOUNDARY_CALL_ENTRY];
  uint64_t args[VKD3D_PE_ENTRY_ARGS];
  unsigned i;

  (void)param;

  for (;;) {
    for (i = 0; i < VKD3D_PE_ENTRY_ARGS; ++i) {
      args[i] = 0;
    }
    args[0] = VKD3D_PUMP_COOKIE_PROCESS;

    if (call_entry(VKD3D_ENTRY_PUMP_WAIT, args) != 1) {
      break; /* shutdown, or a host that does not implement the pump */
    }
    if (args[1]) {
      /* kernel32!SetEvent is a wrapper around exactly this call. A failure
       * here means the game closed the event while a wait was outstanding,
       * which is its bug, not ours -- keep pumping. */
      NtSetEvent((void*)(uintptr_t)args[1], 0);
      __atomic_fetch_add(&vkd3d_shim_pump_events, 1, __ATOMIC_RELAXED);
    }
  }

  __atomic_store_n(&vkd3d_shim_pump_state, 0, __ATOMIC_RELEASE);
  return 0;
}

/* Idempotent, and deliberately never fatal: a device that works but cannot
 * signal fence events is far better than no device, and the failure is
 * visible in vkd3d_shim_pump_state. */
static void StartPump(void) {
  CLIENT_ID id;
  int32_t expected = 0;

  if (!__atomic_compare_exchange_n(&g_PumpStarted, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }

  if (RtlCreateUserThread(CurrentProcess(), 0, 0, 0, 0, 0, (void*)&PumpThreadProc, 0, &g_PumpThread, &id) != STATUS_SUCCESS) {
    __atomic_store_n(&g_PumpStarted, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&vkd3d_shim_pump_state, -1, __ATOMIC_RELEASE);
    g_PumpThread = 0;
    return;
  }
  __atomic_store_n(&vkd3d_shim_pump_state, 1, __ATOMIC_RELEASE);
}

/* Best-effort, non-blocking, and only on an explicit FreeLibrary. See
 * DllMainCRTStartup. */
static void StopPump(void) {
  pfn_thunk_call_entry call_entry;
  uint64_t args[VKD3D_PE_ENTRY_ARGS];
  unsigned i;

  if (__atomic_load_n(&g_PumpStarted, __ATOMIC_ACQUIRE) != 1) {
    return;
  }
  if (__atomic_load_n(&g_InitState, __ATOMIC_ACQUIRE) != 1) {
    return;
  }
  call_entry = (pfn_thunk_call_entry)(uintptr_t)g_Init.boundary[VKD3D_PE_BOUNDARY_CALL_ENTRY];
  if (!call_entry) {
    return;
  }

  for (i = 0; i < VKD3D_PE_ENTRY_ARGS; ++i) {
    args[i] = 0;
  }
  args[0] = VKD3D_PUMP_COOKIE_PROCESS;
  /* Wakes the parked PUMP_WAIT with a shutdown return. By contract this only
   * writes a doorbell, so it does not block -- which is the whole reason it is
   * callable at all from where it is called. The pump thread is NOT joined:
   * waiting for another thread while holding loader lock is the deadlock this
   * design is avoiding. */
  call_entry(VKD3D_ENTRY_PUMP_SHUTDOWN, args);
}

/* --- Exports -------------------------------------------------------------- */

#define DLLEXPORT __declspec(dllexport)

/* pAdapter is forwarded UNCHANGED.
 *
 * docs/d3d12-boundary-analysis.md §1 records that native vkd3d-proton ignores
 * the adapter outright (libs/d3d12core/main.c, `FIXME("Ignoring adapter.")`)
 * and that shim v1 passes NULL. That NULLing belongs in the guest ELF runtime,
 * not here: the adapter a game passes is an IDXGIAdapter from dxvk's DXGI
 * shim, and deciding what it is -- one of our proxies, a foreign proxy, or a
 * real object -- needs the proxy table, which only ppc64le/thunk/runtime has.
 * This file has no way to tell those apart and must not guess.
 *
 * REQUIREMENT ON THE GUEST RUNTIME: its D3D12CreateDevice must not let a
 * non-null, non-unwrappable adapter pointer reach native vkd3d-proton, which
 * would dereference it as a native object. Recorded in README.md as a
 * cross-agent contract. */
DLLEXPORT int32_t NTAPI D3D12CreateDevice(void* pAdapter, uint32_t MinimumFeatureLevel, const void* riid, void** ppDevice) {
  int32_t hr;

  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  hr = ENTRY(VKD3D_PE_D3D12CreateDevice, pfn_D3D12CreateDevice)(pAdapter, MinimumFeatureLevel, riid, ppDevice);

  /* Only on a real device. D3D12CreateDevice(..., NULL) is the documented
   * feature-level probe and returns S_FALSE with nothing created; starting a
   * pump for it would leave a thread parked on a host queue no fence will ever
   * feed. */
  if (hr >= 0 && ppDevice && *ppDevice) {
    StartPump();
  }
  return hr;
}

DLLEXPORT int32_t NTAPI D3D12GetDebugInterface(const void* riid, void** ppvDebug) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12GetDebugInterface, pfn_D3D12GetDebugInterface)(riid, ppvDebug);
}

DLLEXPORT int32_t NTAPI D3D12GetInterface(const void* rclsid, const void* riid, void** ppvDebug) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12GetInterface, pfn_D3D12GetInterface)(rclsid, riid, ppvDebug);
}

DLLEXPORT int32_t NTAPI D3D12CreateRootSignatureDeserializer(const void* pSrcData, uint64_t SrcDataSizeInBytes, const void* pRootSignatureDeserializerInterface,
                                                              void** ppRootSignatureDeserializer) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12CreateRootSignatureDeserializer, pfn_D3D12CreateRootSignatureDeserializer)(
    pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

DLLEXPORT int32_t NTAPI D3D12CreateVersionedRootSignatureDeserializer(const void* pSrcData, uint64_t SrcDataSizeInBytes,
                                                                       const void* pRootSignatureDeserializerInterface,
                                                                       void** ppRootSignatureDeserializer) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12CreateVersionedRootSignatureDeserializer, pfn_D3D12CreateRootSignatureDeserializer)(
    pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

DLLEXPORT int32_t NTAPI D3D12SerializeRootSignature(const void* pRootSignature, uint32_t Version, void** ppBlob, void** ppErrorBlob) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12SerializeRootSignature, pfn_D3D12SerializeRootSignature)(pRootSignature, Version, ppBlob, ppErrorBlob);
}

DLLEXPORT int32_t NTAPI D3D12SerializeVersionedRootSignature(const void* pRootSignature, void** ppBlob, void** ppErrorBlob) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12SerializeVersionedRootSignature, pfn_D3D12SerializeVersionedRootSignature)(pRootSignature, ppBlob, ppErrorBlob);
}

DLLEXPORT int32_t NTAPI D3D12EnableExperimentalFeatures(uint32_t NumFeatures, const void* pIIDs, void* pConfigurationStructs,
                                                        uint32_t* pConfigurationStructSizes) {
  if (!EnsureLoaded()) {
    return E_FAIL;
  }
  return ENTRY(VKD3D_PE_D3D12EnableExperimentalFeatures, pfn_D3D12EnableExperimentalFeatures)(NumFeatures, pIIDs, pConfigurationStructs,
                                                                                              pConfigurationStructSizes);
}

/* Not exported: D3D12CoreCreateLayeredDevice, D3D12CoreGetLayeredDeviceSize,
 * D3D12CoreRegisterLayers.
 *
 * They are Microsoft-internal layering entry points used by the D3D12 debug
 * layer's own loader. vkd3d-proton exports none of them -- libs/d3d12/d3d12.def
 * has exactly the eight above, and a repository-wide grep finds no mention of
 * the three anywhere. dxvk-ppc64le's shim likewise exports exactly the nine
 * DXVK provides and no stubs beyond them, so there is no reference precedent
 * for adding them either. Exporting E_NOTIMPL stubs would make GetProcAddress
 * succeed where it should fail, which is strictly worse than not having them:
 * a caller that probes for them would take the layered path and then fail
 * deeper in. Games do not import them. */

/* --- Entry point ---------------------------------------------------------- */

#define DLL_PROCESS_DETACH 0

/* No CRT, so this is the image entry point rather than DllMain.
 *
 * ATTACH does nothing: the guest ELF library is loaded lazily on the first
 * entry-point call rather than at DLL_PROCESS_ATTACH, because loader lock is
 * the wrong place to be dlopen()ing anything.
 *
 * DETACH is split the way the Windows rules split it, and the split is the
 * whole point:
 *
 *   reserved != NULL  the process is exiting. Windows says do nothing -- other
 *                     threads have already been killed at arbitrary points,
 *                     possibly inside the guest ELF library holding its locks,
 *                     and the kernel is about to reclaim everything anyway. We
 *                     skip, exactly as dxvk-ppc64le's shim skips detach
 *                     teardown entirely.
 *
 *   reserved == NULL  an explicit FreeLibrary while the process lives on. The
 *                     pump thread is parked inside the guest ELF library and
 *                     would fault the moment this image is unmapped, so it
 *                     must at least be told to leave. PUMP_SHUTDOWN only
 *                     writes a host doorbell (vkd3d_thunk_abi.h) so it does
 *                     not block, and the thread is not joined -- waiting on
 *                     another thread under loader lock is the deadlock this
 *                     design exists to avoid.
 *
 * The residual race in the second case is real and is recorded rather than
 * papered over: FreeLibrary can return, and the image be unmapped, before the
 * pump thread has finished returning through PumpThreadProc. Games do not
 * FreeLibrary d3d12.dll; the honest fix is a reference-counted unload path in
 * the guest runtime, which does not exist yet. See README.md, "Known gaps". */
int32_t NTAPI DllMainCRTStartup(void* instance, uint32_t reason, void* reserved) {
  (void)instance;
  if (reason == DLL_PROCESS_DETACH && reserved == 0) {
    StopPump();
  }
  return 1;
}
