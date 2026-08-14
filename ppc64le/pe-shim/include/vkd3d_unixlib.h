/* The PE <-> guest-ELF contract for the D3D12 shim.
 *
 * Included by BOTH sides:
 *   pe/d3d12_shim.c            x86-64 PE,  clang --target=x86_64-windows-gnu
 *   libvkd3d_d3d12_Guest.cpp   x86-64 ELF, clang --target=x86_64-linux-gnu
 *
 * There is exactly one unixlib call, made once. It hands the PE side the guest
 * ELF addresses of the eight flat d3d12.dll entry points and of the boundary
 * functions declared in ppc64le/thunk/runtime/vkd3d_thunk_abi.h, after which
 * the PE exports call them directly as ordinary function pointers and no
 * further unixlib call is ever made.
 *
 * Direct calls are correct here because both sides are x86-64 in one address
 * space. Windows keeps the TEB in %gs and System V Linux keeps thread-local
 * storage in %fs, so a thread can be running Windows code and still make an
 * ordinary call into ELF code -- which is why pre-PE-split Wine worked at all.
 * It also has to be this way: after D3D12CreateDevice returns, the game calls
 * proxy->vtbl[n] directly and those vtable entries are ELF addresses. Routing
 * eight entry points through the unixlib dispatcher while every vtable slot
 * bypasses it would be inconsistent for no benefit.
 *
 * All fields are fixed-width with explicit padding, because the two sides are
 * compiled with different targets.
 */
#pragma once

#include <stdint.h>

/* Index into __wine_unix_call_funcs, which the guest ELF library exports and
 * wine's ntdll dlsym()s (dlls/ntdll/unix/virtual.c, get_unixlib_funcs). */
enum {
  unix_vkd3d_init = 0,
  unix_vkd3d_funcs_count = 1
};

/* Index into vkd3d_unix_init_params::entry. Matches the first eight members of
 * enum vkd3d_thunk_entry in thunk/runtime/vkd3d_thunk_abi.h, in that order --
 * the eight flat exports of d3d12.dll (libs/d3d12/d3d12.def). */
enum {
  VKD3D_PE_D3D12CreateDevice = 0,
  VKD3D_PE_D3D12GetDebugInterface = 1,
  VKD3D_PE_D3D12GetInterface = 2,
  VKD3D_PE_D3D12CreateRootSignatureDeserializer = 3,
  VKD3D_PE_D3D12CreateVersionedRootSignatureDeserializer = 4,
  VKD3D_PE_D3D12SerializeRootSignature = 5,
  VKD3D_PE_D3D12SerializeVersionedRootSignature = 6,
  VKD3D_PE_D3D12EnableExperimentalFeatures = 7,
  VKD3D_PE_ENTRY_COUNT = 8
};

/* Index into vkd3d_unix_init_params::boundary: the guest-side functions of
 * vkd3d_thunk_abi.h. The PE only calls VKD3D_PE_BOUNDARY_CALL_ENTRY (for the
 * fence pump, below); the rest are handed over so a PE-side diagnostic or a
 * future PE-side shim can reach the boundary without a second unixlib call,
 * and so a null in this table is a load-time diagnosis rather than a crash. */
enum {
  VKD3D_PE_BOUNDARY_CALL = 0,        /* vkd3d_thunk_call           */
  VKD3D_PE_BOUNDARY_CALL_FLOAT = 1,  /* vkd3d_thunk_call_float     */
  VKD3D_PE_BOUNDARY_CALL_ENTRY = 2,  /* vkd3d_thunk_call_entry     */
  VKD3D_PE_BOUNDARY_HOST_PROBE = 3,  /* vkd3d_thunk_host_probe     */
  VKD3D_PE_BOUNDARY_SET_ABI_SYSV = 4,/* vkd3d_thunk_set_abi_sysv   */
  VKD3D_PE_BOUNDARY_COUNT = 5
};

/* Set by the PE side in in_magic when the `in` fields below are present.
 * Checked rather than assumed: an OLD PE shim paired with a NEW guest .so
 * leaves whatever its shorter struct happened to hold there, and reading a
 * wild value out of somebody else's stack frame is not a failure mode worth
 * having. Both halves come out of one build.sh run, so this is
 * belt-and-braces, not a supported configuration. */
#define VKD3D_UNIX_INIT_MAGIC 0x564b443344120001ull

/* ---- the fence pump cookie -----------------------------------------------
 *
 * vkd3d_thunk_abi.h describes VKD3D_ENTRY_PUMP_WAIT's args[0] as a "device
 * cookie". The PE shim passes VKD3D_PUMP_COOKIE_PROCESS, and this is a
 * deliberate narrowing that the host stub must be built to match:
 *
 *   - the shim starts exactly ONE pump thread, at the first successful
 *     D3D12CreateDevice, for the life of the process;
 *   - the values the pump hands to SetEvent are guest Win32 HANDLEs, which are
 *     unique process-wide, so no per-device demultiplexing is needed on the
 *     guest side;
 *   - therefore one completion queue and one doorbell serve every device, and
 *     a per-device cookie would only add a demux the guest immediately undoes.
 *
 * The host stub should treat cookie 0 as "every device": the reaper pushes
 * into the single queue no matter which device's fence completed. If the host
 * runtime later wants genuine per-device pumps, the guest ELF runtime -- which
 * owns the proxy table and therefore the only stable device identity -- must
 * hand cookies to the PE through this struct, and this constant becomes the
 * default rather than the only value. That is a one-field change here and one
 * line in d3d12_shim.c's StartPump().
 *
 * Recorded loudly because the alternative -- passing the ID3D12Device proxy
 * pointer -- looks obvious and is WRONG: QI'ing a device for ID3D12Device5
 * yields a different proxy pointer for the same device, so the proxy pointer
 * is not a device identity at all. */
#define VKD3D_PUMP_COOKIE_PROCESS 0ull

/* Number of uint64_t slots the PE side reserves for an args[] block handed to
 * vkd3d_thunk_call_entry. The widest flat entry takes four; PUMP_WAIT uses two
 * (in cookie, out event cookie). Eight is slack, and costs a stack frame. */
#define VKD3D_PE_ENTRY_ARGS 8

struct vkd3d_unix_init_params {
  /* in: VKD3D_UNIX_INIT_MAGIC, or 0 for "the in fields below are absent". */
  uint64_t in_magic;
  /* in: sizeof(struct vkd3d_unix_init_params) as the CALLER understands it.
   * The guest side refuses to write past it. */
  uint32_t in_size;
  uint32_t in_pad;

  /* out: how many of the eight native vkd3d-proton exports the HOST side
   * resolved (vkd3d_thunk_host_probe). 8 means vkd3d-proton is attached and
   * complete; anything less and the PE side fails the call with a named
   * HRESULT instead of letting CreateDevice fail with no explanation. */
  uint32_t resolved;
  /* out: 1 if the guest runtime's generated vtables are in MS-x64 mode (the
   * mode a PE caller needs), 0 if SysV. Diagnostic only -- the PE never
   * changes it, because MS-x64 is the default on x86-64 builds precisely
   * because the deployment caller is a PE. */
  uint32_t abi_is_ms;

  /* out: guest ELF addresses of the eight flat entry points, indexed by
   * VKD3D_PE_*. A zero here means the guest runtime did not export that
   * symbol; the PE side treats it as a hard init failure. */
  uint64_t entry[VKD3D_PE_ENTRY_COUNT];

  /* out: guest ELF addresses of the boundary functions, indexed by
   * VKD3D_PE_BOUNDARY_*. Only CALL_ENTRY is on a hot path (the pump). */
  uint64_t boundary[VKD3D_PE_BOUNDARY_COUNT];
};
