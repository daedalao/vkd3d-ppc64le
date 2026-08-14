# `pe-shim/` — the PE `d3d12.dll`, the FEX thunk pair, and registration

How an emulated x86-64 game's call to `d3d12.dll` reaches **native ppc64le
vkd3d-proton** in the same process.

This is the D3D12 analog of `dxvk-ppc64le/pe-shim/`, which is a working,
measured implementation of the same idea for D3D11 — read
`dxvk-ppc64le/docs/pe-shim-and-registration.md` first; it explains every
mechanism used here and records what was measured on the AC922. This file
records only what is **different**, and what is **true today** for D3D12.

Evidence marks: **[MEASURED]** a command was run on this development box and
its output observed · **[CODE]** read from source at file:line · **[SPEC]**
inference, with the confirming test named · **[DEFERRED]** cannot be done here,
needs the POWER box.

---

## 1. The chain

```
game.exe                   x86-64 PE, emulated by FEX
  d3d12.dll                x86-64 PE shim              pe/d3d12_shim.c
    (wine unixlib, ONCE)
  libvkd3d_d3d12-guest.so  x86-64 guest ELF            libvkd3d_d3d12_Guest.cpp
                                                       + ppc64le/thunk/{generated,runtime}
    (FEX thunk, opcode 0F 3F)
  libvkd3d_d3d12-host.so   native ppc64le              libvkd3d_d3d12_Host.cpp
                                                       + ppc64le/thunk/{generated,runtime}
    (dlopen)
  libvkd3d-proton-d3d12.so native ppc64le vkd3d-proton
   -> libvkd3d-proton-d3d12core.so
```

The seam is Wine's **unixlib loader**: a PE module asking ntdll to `dlopen` a
named Unix `.so` and hand back its `__wine_unix_call_funcs` table. It is the
only sanctioned way an arbitrary PE can reach an arbitrary ELF, and it is used
**exactly once** — the call returns the guest ELF addresses of the eight flat
entry points, and everything afterwards (including every COM vtable slot) is an
ordinary indirect call.

The PE is MS-x64 and the guest ELF is SysV; those disagree from the first
argument onward. Handled by declaring the guest function-pointer types
`__attribute__((sysv_abi))` in `pe/d3d12_shim.c` (`GUESTABI`). The inbound
direction needs no marking — the PE exports are `ms_abi`, which is what a
Windows game calls them with, and the generated COM vtables default to MS-x64
on x86-64 for the same reason.

---

## 2. Files

| File | Role |
|---|---|
| `include/vkd3d_thunk_api.h` | the four thunked host functions, all scalars |
| `include/vkd3d_unixlib.h` | the PE↔ELF contract (one call, one struct) |
| `libvkd3d_d3d12_interface.cpp` | thunkgen interface definition |
| `libvkd3d_d3d12_Guest.cpp` | guest: the four boundary fns + `LOAD_LIB` + `__wine_unix_call_funcs` |
| `libvkd3d_d3d12_Host.cpp` | host: the four `custom_host_impl` forwarders |
| `pe/d3d12_shim.c`, `pe/d3d12.def`, `pe/ntdll.def` | the PE shim |
| `pe/kernel32.def` | import-lib input for the PE test only |
| `build.sh` | out-of-tree build, degrades gracefully (§4) |
| `run-attach.sh` | the exact FEX invocation, with its landmines |
| `tests/attach.c` | guest ELF attach test (no wine needed) |
| `tests/pe_attach.c` | x86-64 PE attach test (needs wine — §6) |
| `ThunksDB-vkd3d_d3d12.json`, `fex-registration.patch` | registration |

Nothing under `ppc64le/thunk/`, `ppc64le/idl/`, `ppc64le/docs/` or
`ppc64le/layout/` is touched by this directory.

---

## 3. What is different from the D3D11 reference

Five things, and each one is a decision rather than an accident.

### 3.1 The thunked surface passes the argument block by ADDRESS

dxvk's four thunked functions flatten `uint64_t args[10]` into ten separate
scalar parameters. These four pass one `uint64_t` holding the guest's `args[]`
address instead.

Both keep the property that matters — **no pointer type appears in the thunked
signature, so thunkgen emits no repacking at all** — because FEX shares one
address space and the host may simply dereference the guest address.

The reason to differ: `vkd3d_thunk_abi.h` specifies the boundary as
`uint64_t *args` with `VKD3D_THUNK_MAX_ARGS 24`, and its census fact ("the
widest slot in the whole surface is exactly 10 parameters") is a measurement of
today's headers, not a guarantee. Flattening would freeze the arity **into the
thunked ABI**, where widening it later costs a new SHA-256 export record and a
FEX rebuild. Passing the address costs one indirection on the host side and
moves the arity question entirely inside the host runtime, where it belongs.
Same argument for `fin`/`fout`: a float shape needing three inputs would not
touch this layer.

### 3.2 There is no WCHAR shim, and there should not be

This is the D3D11 project's single largest component and it **vanishes** for
D3D12, for two independent reasons:

- native vkd3d-proton types `WCHAR` as `unsigned short` [CODE]
  `include/vkd3d_windows.h:89` — two bytes, the same as a real PE — whereas
  DXVK-native uses `wchar_t`, four bytes on Linux. That divergence is what
  forced 36 vtable-slot conversions there;
- the D3D12 API carries no `WCHAR` inside any struct at all
  (`docs/d3d12-boundary-analysis.md` §7); the wide strings that exist
  (`SetName(LPCWSTR)`, the debug-object-name GUID) are pointer parameters.

`ppc64le/layout/` is the mechanical confirmation of the first point and owns
it. **If that probe ever reports a 4-byte `WCHAR` on the native side, this
paragraph is wrong and a conversion layer belongs in the guest ELF library —
not in the PE**, for the reason dxvk documents: the PE is not on the vtable
call path.

### 3.3 The fence pump thread lives here

D3D12's `ID3D12Fence::SetEventOnCompletion(value, hEvent)` has no D3D11
analog. `hEvent` is a guest Win32 HANDLE that vkd3d's own worker threads are
supposed to signal; those are host pthreads, FEX forbids host→guest calls
outside a guest-initiated crossing, and native vkd3d would read the HANDLE bits
as an eventfd [CODE] `include/private/vkd3d_native_sync_handle.h` — doubly
wrong on a box running ntsync, where a Wine event has no fd at all.

`vkd3d_thunk_abi.h` and `docs/d3d12-boundary-analysis.md` §4 resolve it with a
host-side eventfd pool + reaper and **one guest pump thread**. That thread is
in the PE because `SetEvent` on a Wine HANDLE is Win32: neither the guest ELF
library nor native ppc64le code can do it.

- started at the **first successful `D3D12CreateDevice`** (not at
  `DLL_PROCESS_ATTACH`: creating a thread under loader lock is wrong, and
  before a device exists there is nothing to wait on). `D3D12CreateDevice(...,
  NULL)` is the documented feature-level probe and does **not** start it.
- the loop parks inside `vkd3d_thunk_call_entry(VKD3D_ENTRY_PUMP_WAIT, args)`,
  a synchronous crossing that blocks in native code on the host doorbell, and
  comes back either with a cookie to signal (`ret == 1`, cookie in `args[1]`)
  or with a shutdown (`ret == 0`). **Every host→guest transition is the return
  of a call the guest made**, which is what FEX permits.
- it uses `RtlCreateUserThread` and `NtSetEvent` from **ntdll**, not
  `CreateThread`/`SetEvent` from kernel32. `kernel32!CreateThread` forwards to
  `RtlCreateUserThread` and `kernel32!SetEvent` forwards to `NtSetEvent`, so
  nothing is lost, and the shim keeps a one-DLL import table with no run-time
  symbol resolution that could fail silently. [MEASURED] the built DLL imports
  exactly seven ntdll symbols and nothing else.

**The pump cookie is a judgment call, recorded in `include/vkd3d_unixlib.h`.**
`vkd3d_thunk_abi.h` calls `args[0]` a "device cookie"; the shim passes
`VKD3D_PUMP_COOKIE_PROCESS` (0), i.e. one process-wide pump. That is coherent
because exactly one pump thread exists and Win32 HANDLEs are unique
process-wide, so there is nothing to demultiplex. **The obvious alternative —
passing the `ID3D12Device` proxy pointer — is wrong**: QI'ing a device for
`ID3D12Device5` yields a different proxy pointer for the same device, so the
proxy pointer is not a device identity. If the host runtime wants genuine
per-device pumps, the guest runtime must hand cookies to the PE through the
init struct; that is one field here and one line in `StartPump()`.

### 3.4 `D3D12SDKVersion` / `D3D12SDKPath`

Exported as **data**, per `docs/d3d12-boundary-analysis.md` §1.

- `D3D12SDKVersion = 619`, taken from **this tree**, not from the AgilitySDK:
  [CODE] `include/vkd3d_d3d12.idl:381` (`const UINT D3D12_SDK_VERSION = 619`)
  and `ppc64le/idl/gen/vkd3d_d3d12.h:1255`. The number a caller reads out of
  the shim is therefore the number the vkd3d-proton underneath it implements.
  (The task brief suggested 616 as a fallback; the tree says 619, so 619 it
  is.)
- `D3D12SDKPath` is a **`const char*` to a UTF-8 string**, not a `WCHAR`
  string. That is the documented AgilitySDK contract and it is what this
  repository's own [CODE] `tests/d3d12.c:37` exports
  (`const char *D3D12SDKPath = u8".\\D3D12\\"`). The brief asked for
  `L".\\D3D12\\"`; the in-tree evidence overrides it, and getting this wrong
  would hand an Agility-aware loader a UTF-16 buffer where it expects UTF-8.

### 3.5 `D3D12CoreCreateLayeredDevice` &c. are NOT exported

vkd3d-proton exports exactly the eight flat entries [CODE]
`libs/d3d12/d3d12.def`, and a repository-wide grep finds no mention of
`D3D12CoreCreateLayeredDevice`, `D3D12CoreGetLayeredDeviceSize` or
`D3D12CoreRegisterLayers` anywhere [MEASURED]. dxvk-ppc64le's shim likewise
exports exactly the nine DXVK provides and no stubs beyond them, so there is no
reference precedent either. E_NOTIMPL stubs would make `GetProcAddress` succeed
where it should fail, which is strictly worse than absence: a caller probing
for them would take the layered path and fail deeper in. Games do not import
them.

### 3.6 The four boundary functions are dynamically visible

The guest library is built `-fvisibility=hidden`, and dxvk-ppc64le's measured
`nm -D` output shows its three boundary functions are indeed **not** in the
dynamic symbol table — nothing outside its `.so` ever needed their addresses.

Here they are exported (`__attribute__((visibility("default")))`), because the
PE shim's pump calls `vkd3d_thunk_call_entry` and therefore needs it in the
init table, and because `tests/attach.c` and any cross-runtime interop resolve
them by name. The init table itself takes the addresses **directly** rather
than via `dlsym`, since all four are defined in that same translation unit;
`tests/attach.c` asserts that the two routes agree, which is the only way to
catch a symbol that resolves to something other than the function the PE will
call — the PE has no `dlsym` and could never notice.

Left hidden, this would have shown up as a null pointer in the init table at
run time rather than as a compile error, which is why it is called out.

### 3.7 The host runtime's four entry points share their names with the thunked surface

`vkd3d_thunk_abi.h` names the thunkgen surface `vkd3d_host_dispatch`,
`_dispatch_float`, `_entry`, `_probe` — and `ppc64le/thunk/runtime` implements
those names **literally** [CODE] `vkd3d_thunk_host_rt.cpp:411,420,491`,
`generated/vkd3d_thunk_host.cpp`. So the same four C symbols name both the
functions FEX thunks and the functions this library forwards to.

That is fine at the ABI level — a pointer and a `uint64_t` occupy the same GPR
under SysV and ELFv2, which is the premise the whole boundary rests on — but
the two spellings differ: the runtime declares the argument block, the float
inputs and the float output as **pointers**, while the thunked declaration must
spell them as `uint64_t` scalars or thunkgen would repack the pointees (§3.1).

`libvkd3d_d3d12_Host.cpp` handles that **without a pun**: it declares the
runtime's functions with their true types under distinct C++ names, bound to
the real symbols with `__asm__` labels. Nothing is reinterpreted, LTO is safe,
and a rename in the runtime becomes a link error naming the symbol.
[MEASURED] the four asm-labelled declarations produce exactly
`U vkd3d_host_dispatch`, `U vkd3d_host_dispatch_float`, `U vkd3d_host_entry`,
`U vkd3d_host_probe` in the object file, and the host link probe (§4) resolves
all four against the real runtime.

If `ppc64le/thunk` ever changes those four parameters to `uint64_t` — which
costs it nothing and is what the abi header's own prose already says — the asm
labels can simply be deleted.

---

## 4. What builds where

`build.sh` is written to run **in a partially-built world** and says exactly
what it skipped and why. Three things may legitimately be absent and each
degrades to a named TODO rather than a failure: FEX's `thunkgen` binary,
`ppc64le/thunk/{generated,runtime}/*.cpp` (another agent's work), and a ppc64le
host. As of 2026-08-13 the first is absent here and the other two are present,
which is what makes the link probes below possible.

### On any x86-64 box (this development box) — [MEASURED]

```
$ ./build.sh
=== guest ELF library (x86-64)
  no thunkgen; running the LINK PROBE instead (compile everything, expect exactly 4 unresolved)
    fexfn_pack_vkd3d_host_dispatch    fexfn_pack_vkd3d_host_dispatch_float
    fexfn_pack_vkd3d_host_entry       fexfn_pack_vkd3d_host_probe
  ok: the only unresolved symbols are the four thunkgen packers
=== host ppc64le library
  host LINK PROBE (native arch, stub .inl -- proves the symbol contract, not a build artifact):
  ok: links with --no-undefined; the runtime provides all four host entry points
    T vkd3d_host_dispatch  T vkd3d_host_dispatch_float
    T vkd3d_host_entry     T vkd3d_host_probe
=== PE shim (x86-64 COFF)
    build/d3d12.dll: PE32+ executable for MS Windows 6.00 (DLL), x86-64, 5 sections
    D3D12CreateDevice  D3D12CreateRootSignatureDeserializer
    D3D12CreateVersionedRootSignatureDeserializer  D3D12EnableExperimentalFeatures
    D3D12GetDebugInterface  D3D12GetInterface  D3D12SDKPath  D3D12SDKVersion
    D3D12SerializeRootSignature  D3D12SerializeVersionedRootSignature
    (+ six vkd3d_shim_* diagnostics)
  all 10 declared exports present
  imports:  ntdll.dll: LdrGetDllHandle NtQueryVirtualMemory NtSetEvent
            RtlCreateUserThread RtlFindExportedRoutineByName
            RtlInitUnicodeString RtlQueryEnvironmentVariable_U
    build/pe_attach.exe: PE32+ executable for MS Windows 6.00 (console), x86-64
=== attach test program (x86-64 guest ELF)   ok
  build: OK
```

The PE toolchain is `clang --target=x86_64-windows-gnu -nostdlib` plus
`llvm-dlltool` for the import libraries — **no mingw-w64 sysroot**, no CRT, own
`DllMainCRTStartup`. `x86_64-w64-mingw32-dlltool` is accepted as a fallback (an
import library is only a table of names); mingw's *gcc* was not needed and is
not used, so the freestanding/no-CRT discipline is unchanged. `-nostdinc` is
deliberately **not** used: it would remove clang's own resource-directory
headers along with the system ones, and `<stdint.h>` lives there.

**The two link probes are the strongest thing available without `thunkgen`,
and both pass.** [MEASURED] They exist because a syntax check would not have
caught the class of defect that actually matters here — this directory and
`ppc64le/thunk` disagreeing about a symbol.

- **Guest.** Every guest translation unit (this directory's plus
  `thunk/generated/vkd3d_thunk_guest.cpp` and
  `thunk/runtime/vkd3d_proxy.cpp`) is compiled for real, then linked with
  `--no-undefined` against a **stub** `.inl`. The link **must fail**, and it
  must fail on **exactly the four `fexfn_pack_*`** thunkgen would have
  supplied. It does. A `build.sh` that saw the link *succeed* reports a
  failure, because that would mean the stub was being linked in and the probe
  proved nothing.
- **Host.** The host sources are architecture-independent C++, so they compile
  and link on any box. The probe links `libvkd3d_d3d12_Host.cpp` +
  `thunk/generated/vkd3d_thunk_host.cpp` + `thunk/runtime/vkd3d_thunk_host_rt.cpp`
  with `--no-undefined`, and it **succeeds** — which is what proves §3.7's four
  asm-labelled declarations really bind to the runtime's implementations. The
  `.so` it produces is not a build artifact (native arch, no `-mcpu=power8`,
  stub `.inl`) and lives in `build/linkprobe/`.

The stub `.inl` files are hand-derived from `ThunkLibs/Generator/gen.cpp` and
mirror what it emits for four `custom_host_impl` functions — including the
`extern "C"` around the `fexfn_pack_` block (`gen.cpp:612`), without which the
guest probe would report C++-mangled names and prove the wrong thing.

### On the POWER box — [DEFERRED]

Everything else. `build.sh` prints the exact `clang++` command lines for the
host half rather than cross-compiling them; it never cross-compiles ppc64le
here, on purpose.

1. `thunkgen` guest + host `.inl` (needs `$FEXBUILD/Bin/thunkgen`).
2. `libvkd3d_d3d12-guest.so` (x86-64 ELF, cross-compiled into the FEX rootfs).
3. `libvkd3d_d3d12-host.so` (native, `-mcpu=power8`, `--no-undefined`).
4. `./run-attach.sh` — the crossing, under FEX.
5. `wine build/pe_attach.exe` — the PE hop, the one link dxvk-ppc64le never
   closed.

---

## 5. Deployment, and the one non-obvious install step

```sh
install -Dm755 build/libvkd3d_d3d12-host.so  "$FEX_THUNKHOSTLIBS/libvkd3d_d3d12-host.so"
install -Dm755 build/libvkd3d_d3d12-guest.so /usr/local/lib/fex-emu/libvkd3d_d3d12-guest.so
ln -sf <native>/libvkd3d-proton-d3d12.so     "$FEX_THUNKHOSTLIBS/libvkd3d_d3d12.so.0"
install -m755 build/d3d12.dll                <prefix>/drive_c/.../d3d12.dll   # + d3d12=n
```

- FEX loads the host half **purely** from the guest half's `LOAD_LIB`
  constructor: the name `libvkd3d_d3d12` becomes
  `$FEX_THUNKHOSTLIBS/libvkd3d_d3d12-host.so`. No ThunksDB entry is involved on
  the PE path at all.
- the second path is the PE shim's compiled-in default, overridable at run time
  with **`VKD3D_THUNK_GUEST_SO`** (a Unix path, an NT path, or a bare name
  searched in Wine's dll directories all work).
- **the third line is the one that will be forgotten.** thunkgen's generated
  loader always `dlopen`s `<thunk name>.so.<version>` and hands FEX a **null
  export table** when it fails, so without that symlink the thunk fails to load
  with `Failed to initialize thunk library` — *before any D3D12 call happens*,
  which reads like a build problem rather than a missing file. vkd3d-proton's
  native library is `libvkd3d-proton-d3d12.so` and the thunk cannot be named
  that (the name is also the `LOAD_LIB` token and a C identifier prefix), so
  the name is provided as a symlink. `build.sh` creates it in `build/` when it
  can find a native build (`VKD3D_NATIVE=`); `run-attach.sh` puts `build/` on
  `LD_LIBRARY_PATH`. Full reasoning in `libvkd3d_d3d12_interface.cpp`.

`ThunksDB-vkd3d_d3d12.json` is supplied for the **other** direction only: a
guest that `dlopen`s vkd3d-proton by name (an x86-64 Linux build under FEX, or
a Proton shipping it as ELF) gets the thunk substituted.

---

## 6. Running

### `./run-attach.sh` — the ELF caller, POWER box

```sh
export HOME=/tmp/vkd3dthunk-home          # landmine 1
export FEX_ROOTFS=<rootfs>                # landmine 2
export FEX_THUNKHOSTLIBS=$B FEX_THUNKGUESTLIBS=$B
export LD_LIBRARY_PATH=$B:<native>/d3d12:<native>/d3d12core:<native>/vkd3d
export VKD3D_THUNK_D3D12_LIB=<native>/d3d12/libvkd3d-proton-d3d12.so
export VKD3D_THUNK_ABI=sysv               # landmine 3
$FEXBIN $B/attach $B/libvkd3d_d3d12-guest.so
```

Three landmines, the first two inherited from dxvk-ppc64le where they were
found the hard way:

1. **`HOME` is redirected.** With ThunksDB enabled in
   `~/.config/fex-emu/Config.json` but no matching guest library present, FEX
   dies with `SIGTRAP` and no message unless logging is on. A clean `HOME`
   sidesteps the live config entirely.
2. **`FEX_ROOTFS` must then be set explicitly**, because the rootfs is
   otherwise found through `~/.local/share/fex-emu`. Without it FEX says
   *"Invalid or Unsupported elf file … RootFS path set to ''"*, which reads
   like a problem with the program rather than with the configuration.
3. **`VKD3D_THUNK_ABI=sysv`.** The generated vtables default to MS-x64 on
   x86-64 because the deployment caller is a PE game. `tests/attach.c` is an
   ELF caller and must opt back in, or every method call on a returned proxy
   jumps with its arguments in the wrong registers — silently, because every
   value involved is a plausible integer. **The PE path must NOT set this**;
   `tests/attach.c` asserts on `abi_is_ms` so a missed export shows up as a
   named failure rather than as a crash.

`tests/attach.c` also calls `__wine_unix_call_funcs[0]` **directly**, which is
the exact function the PE shim reaches through wine's loader. So the PE hop is
the only untested link rather than the whole init path — and that part needs no
wine at all.

### `wine build/pe_attach.exe` — the PE caller, POWER box

Run from a directory containing `d3d12.dll`. It loads the shim by the normal
loader, resolves all ten exports, reads `D3D12SDKVersion` through the export
table, crosses via `D3D12EnableExperimentalFeatures(0, NULL, NULL, NULL)`, and
prints the shim's own diagnostics — including **which unixlib route this wine
offered**:

- `vkd3d_shim_unixlib_route == 1002` — this wine has
  `MemoryWineLoadUnixLibByName`. That single number answers the question
  dxvk-ppc64le recorded as its open gap #2.
- `== 1000` — it does not, and the builtin fallback was tried. That route needs
  the shim installed as a Wine **builtin** (a `d3d12.dll` + `d3d12.so` pair in
  a `WINEDLLPATH` directory with `d3d12=b`); dxvk-ppc64le's `run-pe-wine.sh`
  documents the stamping involved.
- `== 0` — init never got as far as the unixlib call.

---

## 7. The contract with `ppc64le/thunk/` (cross-agent)

This directory implements only the crossing. Six things it **requires** of the
runtime. Items 1–4 were **checked against the sources as they landed on
2026-08-13** and all four already hold; 5 and 6 cannot be checked here.

1. ✅ **The guest ELF library must export the eight flat entry points under
   their D3D12 names**, with **SysV** convention, **default visibility** (the
   library is built `-fvisibility=hidden`, so they need the attribute), and the
   signatures of `libs/d3d12/main.c`. `unixcall_init` resolves them with
   `dlsym(RTLD_DEFAULT, …)` and fails with `STATUS_ENTRYPOINT_NOT_FOUND` if any
   is missing.
   [CODE] `thunk/runtime/vkd3d_proxy.cpp:650,718,724,730,738,749,767,784` —
   all eight, all `VKD3D_EXPORT` (= `visibility("default")`), and every
   signature matches the PE-side typedefs argument for argument.
2. ✅ **The host runtime must define four symbols**: `vkd3d_host_dispatch`,
   `vkd3d_host_dispatch_float`, `vkd3d_host_entry`, `vkd3d_host_probe`.
   [CODE] `thunk/runtime/vkd3d_thunk_host_rt.cpp:411,420,491` and
   `thunk/generated/vkd3d_thunk_host.cpp`. They are spelled with pointer
   parameters where the thunked declaration uses scalars — see §3.7 for how
   that is bridged, and §4 for the link probe that proves it.
3. ✅ **`vkd3d_thunk_host_probe` is defined on the GUEST side by this
   directory** (it is nothing but a crossing); `vkd3d_thunk_abi.h:69` declares
   it and nothing in `ppc64le/thunk` defines it. If that changes, delete the
   one in `libvkd3d_d3d12_Guest.cpp` — the linker will say so.
4. ✅ **`D3D12CreateDevice`'s adapter must be neutralised by the guest runtime,
   not by the PE.** The shim forwards `pAdapter` unchanged, deliberately: the
   adapter a game passes is an `IDXGIAdapter` from dxvk's DXGI shim, and
   telling "one of our proxies" from "a foreign proxy" from "a real object"
   needs the proxy table, which only `ppc64le/thunk/runtime` has. Native
   vkd3d-proton ignores the adapter entirely [CODE]
   `libs/d3d12core/main.c:687` (`FIXME("Ignoring adapter.")`).
   [CODE] `thunk/runtime/vkd3d_proxy.cpp:661-677` already forces `a[0] = 0` and
   warns once. Nothing to do.
5. ❓ **The host stub must accept pump cookie 0 as "every device"** (§3.3), and
   `VKD3D_ENTRY_PUMP_SHUTDOWN` must not block — the PE calls it from
   `DLL_PROCESS_DETACH`. The runtime has `vkd3d_host_pump_dispatch` /
   `pump_wait` / `pump_shutdown`, so the machinery exists; whether it keys on
   the cookie has not been read here.
6. ❓ **The generated guest vtables must default to MS-x64 on x86-64.** The PE
   never calls `vkd3d_thunk_set_abi_sysv`, and `tests/attach.c` (an ELF caller)
   asserts the opposite via `abi_is_ms`, so a wrong default fails one of the
   two tests loudly rather than corrupting register assignment silently.

---

## 8. Status, honestly

| Artifact | State |
|---|---|
| `d3d12.dll` — x86-64 PE shim | **builds and links, PE32+, all 10 exports, ntdll-only imports** [MEASURED]; **never executed** — no x86-64 Wine here |
| `pe_attach.exe` — x86-64 PE test | **builds and links, PE32+ console** [MEASURED]; never executed |
| `attach` — x86-64 ELF test | **builds** [MEASURED]; never executed (needs the guest `.so`) |
| `libvkd3d_d3d12_Guest.cpp` | **compiles, and links against the real `ppc64le/thunk` guest sources with exactly the four thunkgen packers unresolved** [MEASURED]; never compiled against a real `.inl` |
| `libvkd3d_d3d12_Host.cpp` | **compiles, and links `--no-undefined` against the real `ppc64le/thunk` host sources** [MEASURED] — the four host entry points resolve |
| `libvkd3d_d3d12-guest.so` / `-host.so` | **do not exist** — need `thunkgen` (and, for the host, a ppc64le box) |
| the cross-agent contract (§7 items 1–4) | **checked against the landed `ppc64le/thunk` sources; all four already hold** [CODE] |
| `fex-registration.patch` | **verified to apply cleanly** against `fex-ppc64le` as of 2026-08-13, and the resulting `ThunksDB.json` parses [MEASURED]; not applied |
| `ThunksDB-vkd3d_d3d12.json` | valid JSON [MEASURED] |

**Nothing in this directory has ever executed.** The D3D11 project got as far as
a real crossing into native DXVK; this one cannot yet, because `thunkgen` is not
built here and the box it must run on is elsewhere. What *is* established is
that the pieces fit: every symbol this directory expects from `ppc64le/thunk`
exists with the right name, linkage and signature, and every symbol it owes is
present.

### Known gaps

1. **The PE shim has never been executed**, and neither has the guest half.
   Everything above is a build-time result. `tests/pe_attach.c` exists so that
   closing this is one command on the POWER box.
2. **`MemoryWineLoadUnixLibByName` (1002) is not present in every wine.**
   dxvk-ppc64le [MEASURED] GE-Proton11-3's wine-11.0 refusing it while
   answering 1000/1001. Both routes are wired; `pe_attach.exe` reports which
   one ran. `STATUS_INVALID_PARAMETER` is treated as a second "unknown class"
   status alongside `STATUS_INVALID_INFO_CLASS`, because wine's
   `NtQueryVirtualMemory` has returned either depending on version — a
   divergence from the reference, which checks only the latter and would fall
   through to a hard failure on such a wine.
3. **`RtlQueryEnvironmentVariable_U` semantics are assumed** [SPEC] — that it
   fills `value.Length` in bytes and does not NUL-terminate. If wrong, the shim
   falls back to the compiled-in default path (benign) but
   `VKD3D_THUNK_GUEST_SO` would silently not work. `pe_attach.exe` confirms it
   in passing.
4. **`RtlCreateUserThread`/`NtSetEvent` are assumed present and Wine-correct**
   [SPEC]. They are ntdll exports on every Wine, and `kernel32!CreateThread` /
   `!SetEvent` forward to them; if the assumption is wrong the pump fails to
   start, `vkd3d_shim_pump_state` reads `-1`, and everything except
   event-based fence waits still works.
5. **`FreeLibrary`-time teardown races.** `DLL_PROCESS_DETACH` with
   `reserved == NULL` fires `PUMP_SHUTDOWN` and does **not** join the pump
   thread — joining under loader lock is the deadlock this whole design avoids.
   So `FreeLibrary` can return, and the image be unmapped, before the pump
   thread finishes returning. Games do not `FreeLibrary` `d3d12.dll`; the
   honest fix is a refcounted unload path in the guest runtime, which does not
   exist. On process exit (`reserved != NULL`) nothing is done at all, exactly
   as the reference skips detach teardown.
6. **`__wine_unix_call_funcs` has one entry and there is no unload path.** A
   process that `FreeLibrary`s `d3d12.dll` leaks the guest ELF library and its
   host thunk. Recorded because the `res[0]` dlopen handle is captured and then
   unused, which otherwise reads like an oversight.
7. **The float path is written but unexercised**, as in the reference. D3D12
   has roughly two float-class prototypes (`ClearDepthStencilView`,
   `OMSetDepthBounds`) against D3D11's five.
8. **The `.inl` stubs used for the link probes are hand-derived from
   `gen.cpp`.** They match what the generator emits for four
   `custom_host_impl` functions today; a thunkgen change would not be caught
   until a real build runs. This is the one thing the probes cannot cover, by
   construction.
9. **The `libvkd3d_d3d12.so.0` symlink is an unguarded install step** (§5). If
   it is missing the whole thunk fails to load, with a message that does not
   mention it.
10. **The pump cookie contract is unverified on the host side** (§7 item 5).
    The runtime has `pump_wait`/`pump_shutdown`; whether it treats cookie 0 as
    "every device" was not read. If it demands a real per-device cookie the
    pump will park forever on an empty queue — visible as
    `vkd3d_shim_pump_events` staying 0 while fences complete.
11. **`build.sh` compiles another agent's in-flight sources** for the two link
    probes. If `ppc64le/thunk` is mid-edit the probes can fail for reasons that
    have nothing to do with this directory; the log paths are printed, and the
    probe steps are the only ones that touch those files.
