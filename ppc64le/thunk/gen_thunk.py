#!/usr/bin/env python3
"""Generate the guest and host halves of the D3D12 COM thunk from interfaces.json.

The D3D12 analog of dxvk-ppc64le's thunk/gen_thunk.py, and deliberately the
same shape.  Read ppc64le/docs/d3d12-boundary-analysis.md first; it records
what D3D12 changes relative to the proven D3D11 design, and every deviation
below points back at it.

## Why this can be generated at all

1. **Vtable slots are derivable.**  gen_interfaces.py resolves all 76
   interfaces out of the widl-generated C headers, so (interface, method) ->
   slot is computed, not hand-maintained.  A hand-written table that drifts by
   one dispatches to the neighbouring method with the neighbour's argument
   types, and compiles cleanly.

2. **Every argument is integer-class and fits 64 bits** -- with exactly two
   classes of exception, both of which this generator finds mechanically and
   refuses to guess about:
     * by-value FLOAT parameters (37 instances across 3 prototypes / 23 vtable
       slots), which both ABIs place in FP registers.  They ride
       vkd3d_thunk_call_float with a shape id and hand-written stubs.
     * by-value aggregates larger than 8 bytes: exactly two slots take
       D3D12_NODE_ID (16 bytes) by value, and single-uint64 transport is WRONG
       for them under every ABI.  They are REFUSED.
   Everything else by value is <= 8 bytes and integer-class -- including
   D3D12_CPU_DESCRIPTOR_HANDLE (192 uses) and D3D12_GPU_DESCRIPTOR_HANDLE (44),
   which MS-x64 passes in one GPR, SysV classifies as one INTEGER eightbyte and
   ELFv2 passes in a GPR.  check_invariants() re-derives all of that from
   interfaces.json and fails generation rather than emitting a table that is
   quietly wrong.

3. **Aggregate returns need no new transport.**  91 slots return a struct by
   value in the IDL; the widl C vtable that native vkd3d-proton implements gives
   those slots an explicit `RET *__ret` parameter right after `This` and returns
   that pointer -- the same convention an MSVC-compiled caller uses.  They ride
   the generic path as (this, retptr, args...) -> retptr.  The only thing this
   generator does about them is give the ms_abi forwarder a pointer return type
   instead of uint64_t.

## What is generated and what is not

Generated: the slot tables, three arrays per interface, per-slot argument
packing and interface-pointer marshalling, the IID -> interface id table, the
per-slot flag inventory, and the generic host dispatcher.

Hand-written, in runtime/: everything the "pack into uint64_t[10]" convention
cannot express -- slots 0/1/2 of every interface (interning and reference
counting are policy, not marshalling), the three float-class shapes, the eight
flat entry points, and the host-side fence/event pump.

Usage:
  ./gen_thunk.py --out generated/
"""

import argparse
import json
import os
import re
import sys
import uuid

# --------------------------------------------------------------------------
# The uint64_t[N] argument block.  The widest slot in the whole surface is 10
# parameters (ID3D12CommandQueue::UpdateTileMappings and
# ID3D12Device10::CreateCommittedResource3), which check_invariants() re-derives
# rather than trusting.  VKD3D_THUNK_MAX_ARGS in runtime/vkd3d_thunk_abi.h is
# the transport's ceiling (24); this is what the stubs actually allocate.
# --------------------------------------------------------------------------
MAX_ARGS = 10

# --------------------------------------------------------------------------
# Float-class methods.  check_invariants() proves this list complete against
# interfaces.json in both directions, so it is a table of *prototypes*, not a
# filter: a new float-class method in a regenerated census FAILS generation
# instead of silently taking the integer path and reading an unrelated register.
#
#   (declaring interface, method) -> (shape enum, hand-written stub symbol)
# --------------------------------------------------------------------------
FLOAT_METHODS = {
    ("ID3D12GraphicsCommandList", "ClearDepthStencilView"):
        ("VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW", "vkd3d_fstub_ClearDepthStencilView"),
    ("ID3D12GraphicsCommandList1", "OMSetDepthBounds"):
        ("VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS", "vkd3d_fstub_OMSetDepthBounds"),
    ("ID3D12GraphicsCommandList9", "RSSetDepthBias"):
        ("VKD3D_FSHAPE_RS_SET_DEPTH_BIAS", "vkd3d_fstub_RSSetDepthBias"),
}

FLOAT_SHAPE_ORDER = [
    "VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW",
    "VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS",
    "VKD3D_FSHAPE_RS_SET_DEPTH_BIAS",
]

# The REAL prototype of each shape, after `Proxy* self`.  The ms_abi forwarder
# is emitted from this and must be exact: MS-x64 assigns an XMM register by
# argument POSITION, so ClearDepthStencilView's `depth` -- the fourth argument,
# because the by-value descriptor handle occupies an integer position -- arrives
# in XMM3 while SysV puts the first float in XMM0.  A generic uint64_t forwarder
# would drop it.
#   (return type, [(C type, name, kind)]) where kind is "i" integer-class,
#   "f" float-class, or the name of a by-value aggregate type.
FLOAT_PROTO = {
    "VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW":
        ("void", [("VkdCpuDescriptorHandle", "p0", "D3D12_CPU_DESCRIPTOR_HANDLE"),
                  ("uint32_t", "p1", "i"),
                  ("float", "p2", "f"),
                  ("uint8_t", "p3", "i"),
                  ("uint32_t", "p4", "i"),
                  ("const void*", "p5", "i")]),
    "VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS":
        ("void", [("float", "p0", "f"), ("float", "p1", "f")]),
    "VKD3D_FSHAPE_RS_SET_DEPTH_BIAS":
        ("void", [("float", "p0", "f"), ("float", "p1", "f"), ("float", "p2", "f")]),
}

MAX_FLOAT_ARGS = 3

# --------------------------------------------------------------------------
# By-value aggregates <= 8 bytes.  Transport is a single uint64_t slot, which is
# correct for MS-x64 (any 1/2/4/8-byte aggregate in one GPR), SysV (one INTEGER
# eightbyte) and ELFv2 (small aggregate in a GPR).  The ms_abi forwarders and
# the host dispatch nevertheless declare them with a POD of the REAL shape and
# bit-copy through a union, so the compiler -- not this generator -- decides the
# register class.  Sizes were measured against the widl headers, not assumed;
# see check_invariants() and README.md.
#   census name -> (C++ POD emitted into vkd3d_thunk_ids.h, member declaration)
# --------------------------------------------------------------------------
BYVAL_AGGREGATES = {
    "D3D12_CPU_DESCRIPTOR_HANDLE": ("VkdCpuDescriptorHandle", "uint64_t ptr;"),
    "D3D12_GPU_DESCRIPTOR_HANDLE": ("VkdGpuDescriptorHandle", "uint64_t ptr;"),
    "LUID":                        ("VkdLuid", "uint32_t LowPart; int32_t HighPart;"),
}

# By-value aggregates LARGER than 8 bytes.  Transport-incompatible: MS-x64
# passes them by hidden reference, SysV/ELFv2 in two registers, so no single
# uint64_t slot can carry one.  Slots taking these are refused outright.
BYVAL_REFUSED = {
    "D3D12_NODE_ID": "16-byte {LPCWSTR,UINT} passed by value; MS-x64 passes it "
                     "by hidden reference and SysV/ELFv2 in two registers, so "
                     "single-slot transport is wrong under every ABI",
}

# --------------------------------------------------------------------------
# Slots refused for reasons other than a by-value aggregate.  A refused stub
# warns once with the method name and returns a poison value WITHOUT crossing.
#
# RegisterDestructionCallback takes a GUEST function pointer that native
# vkd3d-proton would call from its own host threads at object destruction.  FEX
# forbids host->guest calls outside a guest-initiated crossing, so honouring it
# is not a marshalling problem but an impossible one; the boundary analysis
# (§3, ID3D12InfoQueue1::RegisterMessageCallback) already fixes the policy for
# this class: accept-and-warn or E_NOTIMPL, never call.  Dead entries fail
# generation, same as everywhere else here.
# --------------------------------------------------------------------------
REFUSED_SLOTS = {
    ("ID3DDestructionNotifier", "RegisterDestructionCallback"):
        "takes a guest function pointer that vkd3d would call from a host "
        "thread; FEX forbids host->guest calls outside a guest-initiated "
        "crossing",
}

# --------------------------------------------------------------------------
# Interface-pointer ARRAY parameters.
#
# An `Iface **pp` is either ONE out-parameter or an ARRAY of them and nothing in
# the C declaration says which; the count lives in a different argument.
# Guessing is a memory-safety decision, so every array-capable parameter of a
# method that also declares a count-like argument must appear here explicitly.
# check_invariants() FAILS GENERATION on an unclassified one, and on an entry
# here that no longer matches a real parameter.
#
#   ("arg", j)   element count is argument j, by value
#   ("deref", j) element count is *(const UINT*)argument j, read before the call
#   "single"     exactly one element despite a count-like sibling argument
#   "refuse"     cannot be marshalled
# --------------------------------------------------------------------------
ARRAY_SPECS = {
    ("ID3D12CommandQueue", "ExecuteCommandLists"):              {1: ("arg", 0)},
    ("ID3D12Device", "MakeResident"):                           {1: ("arg", 0)},
    ("ID3D12Device", "Evict"):                                  {1: ("arg", 0)},
    ("ID3D12Device1", "SetEventOnMultipleFenceCompletion"):     {0: ("arg", 2)},
    ("ID3D12Device1", "SetResidencyPriority"):                  {1: ("arg", 0)},
    ("ID3D12Device3", "EnqueueMakeResident"):                   {2: ("arg", 1)},
    ("ID3D12GraphicsCommandList", "SetDescriptorHeaps"):        {1: ("arg", 0)},
    ("ID3D12GraphicsCommandList1", "AtomicCopyBufferUINT"):     {5: ("arg", 4)},
    ("ID3D12GraphicsCommandList1", "AtomicCopyBufferUINT64"):   {5: ("arg", 4)},
}

# --------------------------------------------------------------------------
# `void **` out-parameters that are NOT riid-driven interface outs.  Enumerated
# by (interface, method, argument index) rather than pattern-matched, because
# the difference between "mapped memory" and "an interface" is not in the
# declaration.
#
# ID3D12Resource::Map's `void **data` is a pointer to mapped device memory that
# the guest writes through directly.  DIVERGENCE FROM THE D3D11 REFERENCE: dxvk
# flags its one raw void** (GetDecoderBuffer) as untranslated so STRICT reports
# it, because the shared-address-space argument had not been established there.
# Here it has -- §5 of the boundary analysis, and it is the same property every
# OUT parameter in this design already relies on -- so Map is correct by design
# and vkd3d_slot_untranslated() deliberately does NOT report RAW_VOID.  The flag
# still exists, so the inventory can be counted.
# --------------------------------------------------------------------------
RAW_VOID_OUT = {
    ("ID3D12Resource", "Map"): {2},
}

# --------------------------------------------------------------------------
# Aggregates that carry interface pointers behind a `void *`, so no member scan
# can see them.  Both are subobject streams whose entries name a struct type at
# run time: D3D12_PIPELINE_STATE_STREAM_DESC's stream carries an
# ID3D12RootSignature* subobject, and D3D12_STATE_OBJECT_DESC's
# D3D12_STATE_SUBOBJECT array points at D3D12_GLOBAL_ROOT_SIGNATURE /
# D3D12_LOCAL_ROOT_SIGNATURE / D3D12_EXISTING_COLLECTION_DESC, all of which do.
# Written down because the mechanical scan below CANNOT find them; dead entries
# fail generation, so they cannot rot into decoration.
# --------------------------------------------------------------------------
OPAQUE_IFACE_STRUCTS = {
    "D3D12_PIPELINE_STATE_STREAM_DESC":
        "void* subobject stream carrying an ID3D12RootSignature* subobject",
    "D3D12_STATE_OBJECT_DESC":
        "D3D12_STATE_SUBOBJECT array whose void* pDesc reaches "
        "D3D12_GLOBAL_ROOT_SIGNATURE / D3D12_LOCAL_ROOT_SIGNATURE / "
        "D3D12_EXISTING_COLLECTION_DESC",
}

# The widl headers the struct scan reads.  Same three the census reads.
IDL_HEADERS = ("vkd3d_d3d12.h", "vkd3d_d3d12sdklayers.h", "vkd3d_d3dcommon.h")

# --------------------------------------------------------------------------
# Host-side slot overrides: the fence/event pump (runtime/vkd3d_thunk_abi.h,
# "fence/event pump").  Derived by (declaring interface, method) so that every
# interface inheriting the slot is covered and the slot NUMBER is never written
# down by hand.  check_invariants() additionally asserts each override's
# parameter shape, because the host override indexes args[] positionally.
#   (owner, method) -> (kind enum, [(arg index, expected base type)])
# --------------------------------------------------------------------------
PUMP_SLOTS = {
    ("ID3D12Fence", "SetEventOnCompletion"):
        ("VKD3D_PUMP_FENCE_EVENT", [(0, "UINT64"), (1, "HANDLE")]),
    ("ID3D12Device1", "SetEventOnMultipleFenceCompletion"):
        ("VKD3D_PUMP_MULTI_FENCE_EVENT",
         [(0, "ID3D12Fence"), (1, "UINT64"), (2, "UINT"),
          (3, "D3D12_MULTIPLE_FENCE_WAIT_FLAGS"), (4, "HANDLE")]),
}

PUMP_KIND_ORDER = ["VKD3D_PUMP_NONE", "VKD3D_PUMP_FENCE_EVENT",
                   "VKD3D_PUMP_MULTI_FENCE_EVENT"]

# --------------------------------------------------------------------------
# Slot numbers the tests drive, emitted as named constants so a regenerated
# interfaces.json cannot leave a test calling the neighbouring method with this
# method's arguments.  Generation fails if one disappears.
# --------------------------------------------------------------------------
NAMED_SLOTS = [
    ("ID3D12Device", "CreateCommandQueue"),
    ("ID3D12Device", "CreateCommittedResource"),
    ("ID3D12Device", "GetDescriptorHandleIncrementSize"),
    ("ID3D12Device", "GetAdapterLuid"),
    ("ID3D12Device10", "CreateCommittedResource3"),
    ("ID3D12Device1", "SetEventOnMultipleFenceCompletion"),
    ("ID3D12DescriptorHeap", "GetCPUDescriptorHandleForHeapStart"),
    ("ID3D12DescriptorHeap", "GetGPUDescriptorHandleForHeapStart"),
    ("ID3D12CommandQueue", "ExecuteCommandLists"),
    ("ID3D12CommandQueue", "Signal"),
    ("ID3D12GraphicsCommandList", "SetGraphicsRootDescriptorTable"),
    ("ID3D12GraphicsCommandList", "ClearRenderTargetView"),
    ("ID3D12GraphicsCommandList", "ResourceBarrier"),
    ("ID3D12GraphicsCommandList", "SetDescriptorHeaps"),
    ("ID3D12GraphicsCommandList", "Close"),
    ("ID3D12Fence", "SetEventOnCompletion"),
    ("ID3D12Fence", "GetCompletedValue"),
    ("ID3D12Resource", "Map"),
    ("ID3D12Resource", "GetDevice"),
    ("ID3D12WorkGraphProperties", "GetNodeIndex"),
    ("ID3D12StateObjectProperties", "GetShaderIdentifier"),
]

# kVkdSlotFlags bits.
FLAG_IN_IFACE     = 1      # takes an Iface*
FLAG_OUT_IFACE    = 2      # writes an Iface** (statically typed)
FLAG_IFACE_ARRAY  = 4      # takes/returns an Iface array plus a count
FLAG_VOID_OUT     = 8      # riid-driven void** out
FLAG_HAND         = 16     # served by a hand-written runtime/ symbol
FLAG_MARSHALLED   = 32     # the generated stub translates every one of the above
FLAG_RAW_VOID     = 64     # void** that is memory, not an interface
FLAG_REFUSED      = 128    # refused rather than passed raw
FLAG_STRUCT_IFACE = 256    # by-pointer struct with interface members inside it
FLAG_AGG_RETURN   = 512    # explicit __ret pointer, returned back
FLAG_BYVAL_AGG    = 1024   # takes a by-value aggregate (<= 8 bytes)
FLAG_PUMP         = 2048   # host-side slot override (fence/event pump)

HEADER = """/* GENERATED by gen_thunk.py -- do not edit.
 *
 * Regenerate with:  ppc64le/thunk/gen_thunk.py --out generated/
 */
"""

# Integer-class scalars and typedefs that appear BY VALUE anywhere in the
# surface.  FAIL-CLOSED: check_invariants() rejects any by-value base type not
# in here (and not float-class, and not a known aggregate), because defaulting
# an unknown type to the integer path is exactly how a float typedef or a
# float-bearing aggregate would read an unrelated register.  Sizes were measured
# against the widl headers: every one is <= 8 bytes.
BYVAL_INTEGER = {
    # plain scalars and typedef'd pointers
    "UINT", "INT", "int", "UINT8", "UINT32", "UINT64", "SIZE_T", "DWORD",
    "ULONG", "BOOL", "HRESULT", "HANDLE", "LPCSTR", "LPCWSTR",
    "REFIID", "REFGUID", "REFCLSID", "PFN_DESTRUCTION_CALLBACK",
    "D3D12_GPU_VIRTUAL_ADDRESS",
    # enums (4 bytes each, measured)
    "D3D_FEATURE_LEVEL", "D3D_ROOT_SIGNATURE_VERSION",
    "D3D12_BACKGROUND_PROCESSING_MODE", "D3D12_BARRIER_LAYOUT",
    "D3D12_CLEAR_FLAGS", "D3D12_COMMAND_LIST_FLAGS", "D3D12_COMMAND_LIST_TYPE",
    "D3D12_DESCRIPTOR_HEAP_TYPE", "D3D12_DEVICE_FACTORY_FLAGS",
    "D3D12_DRED_ENABLEMENT", "D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS",
    "D3D12_FEATURE", "D3D12_FENCE_FLAGS", "D3D12_HEAP_FLAGS", "D3D12_HEAP_TYPE",
    "D3D12_INDEX_BUFFER_STRIP_CUT_VALUE", "D3D12_LIFETIME_STATE",
    "D3D12_MEASUREMENTS_ACTION", "D3D12_META_COMMAND_PARAMETER_STAGE",
    "D3D12_MULTIPLE_FENCE_WAIT_FLAGS", "D3D12_PREDICATION_OP",
    "D3D12_PRIMITIVE_TOPOLOGY", "D3D12_PROTECTED_SESSION_STATUS",
    "D3D12_QUERY_HEAP_FLAGS", "D3D12_QUERY_TYPE",
    "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE",
    "D3D12_RENDER_PASS_FLAGS", "D3D12_RESIDENCY_FLAGS", "D3D12_RESOLVE_MODE",
    "D3D12_RESOURCE_STATES", "D3D12_SERIALIZED_DATA_TYPE",
    "D3D12_SHADER_CACHE_CONTROL_FLAGS", "D3D12_SHADER_CACHE_KIND_FLAGS",
    "D3D12_SHADING_RATE", "D3D12_TILE_COPY_FLAGS", "D3D12_TILE_MAPPING_FLAGS",
    "DXGI_FORMAT",
}


class GenError(Exception):
    pass


def load(path):
    with open(path) as fh:
        return json.load(fh)["interfaces"]


def param_base(p):
    """(pointer depth, base type token) for a C parameter declaration.

    An array declarator counts as one level of indirection, because that is what
    it decays to -- which is why `const FLOAT color[4]` is a pointer and not a
    float-class parameter."""
    stars = p.count("*") + (1 if "[" in p else 0)
    toks = [t for t in re.sub(r"\[.*?\]", " ", p).replace("*", " * ").split()
            if t not in ("const", "*")]
    return stars, (toks[0] if toks else "")


def param_name(p):
    m = re.search(r"([A-Za-z_]\w*)\s*(\[.*\])?\s*$", p)
    return m.group(1) if m else ""


def ret_is_pointer(r):
    return "*" in r


def is_float_class(tok):
    return tok in ("FLOAT", "float", "double", "DOUBLE")


def count_like(p):
    """A by-value integer whose name reads like an element count."""
    stars, b = param_base(p)
    if stars or b not in ("UINT", "UINT32", "int", "INT", "SIZE_T"):
        return False
    n = param_name(p)
    return bool(re.match(r"(?i)^num", n) or re.search(r"(?i)count$", n))


def iface_enum(tok):
    return "VKD3D_IFACE_" + tok.upper()


# ---------------------------------------------------------------------------
# The struct-with-interface scan.
#
# Parses every `typedef struct/union NAME { ... } NAME;` out of the widl headers
# and marks the ones that reach an interface pointer, recursively through nested
# and anonymous aggregates.  Done here, at generation time, rather than written
# into a hand list: the surface is 352 aggregates and the answer must not depend
# on anyone's eyes.  The vtable structs widl also emits (`<Iface>Vtbl`) are
# skipped -- they are full of interface pointers by construction and are not
# parameters.
# ---------------------------------------------------------------------------
def parse_aggregates(gen_dir):
    aggs = {}
    for h in IDL_HEADERS:
        path = os.path.join(gen_dir, h)
        if not os.path.exists(path):
            raise GenError("widl header %s not found; --idl-gen points at %s"
                           % (h, gen_dir))
        with open(path, errors="ignore") as fh:
            text = fh.read()
        for m in re.finditer(r"typedef\s+(struct|union)\s+(\w+)\s*\{", text):
            start = m.end() - 1
            depth = 0
            i = start
            while i < len(text):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[start + 1:i]
            tail = re.match(r"\s*(\w+)\s*;", text[i + 1:i + 200])
            name = tail.group(1) if tail else m.group(2)
            if name.endswith("Vtbl"):
                continue
            aggs[name] = body
    return aggs


def iface_bearing_structs(gen_dir, iface_names):
    aggs = parse_aggregates(gen_dir)
    memo = {}

    def why(name, stack=()):
        if name in memo:
            return memo[name]
        if name in stack or name not in aggs:
            return None
        body = aggs[name]
        for m in re.finditer(r"\b(I[A-Z]\w*)\s*\*", body):
            if m.group(1) in iface_names:
                memo[name] = m.group(1) + " *"
                return memo[name]
        for tok in sorted(set(re.findall(r"\b([A-Za-z_]\w*)\b", body))):
            if tok in aggs and tok != name:
                sub = why(tok, stack + (name,))
                if sub:
                    memo[name] = "%s -> %s" % (tok, sub)
                    return memo[name]
        memo[name] = None
        return None

    found = {}
    for n in sorted(aggs):
        w = why(n)
        if w:
            found[n] = w
    for n, w in OPAQUE_IFACE_STRUCTS.items():
        if n not in aggs:
            raise GenError("OPAQUE_IFACE_STRUCTS names %s, which is not an "
                           "aggregate in the widl headers" % n)
        found[n] = "opaque: " + w
    return found, len(aggs)


# ---------------------------------------------------------------------------
def hand_written(name, slot):
    """Symbol serving this slot from runtime/, or None to generate a stub."""
    if slot["slot"] < 3:
        return ["vkd3d_proxy_qi", "vkd3d_proxy_addref",
                "vkd3d_proxy_release"][slot["slot"]]
    key = (slot["owner"], slot["name"])
    if key in FLOAT_METHODS:
        return FLOAT_METHODS[key][1]
    return None


def classify(iface_names, struct_iface, iface, slot):
    """Marshalling plan for one slot.

    Returns (flags, plan).  Every interface-pointer parameter lands in exactly
    one bucket, or generation fails in check_invariants()."""
    key = (slot["owner"], slot["name"])
    spec = ARRAY_SPECS.get(key, {})
    raw = RAW_VOID_OUT.get(key, set())
    params = slot["params"]
    has_count = any(count_like(p) for p in params)

    plan = {"in": [], "out": [], "riid": [], "in_arr": [], "out_arr": [],
            "raw": [], "refuse": [], "struct": [], "byval": []}
    flags = 0
    if slot.get("aggregate_return"):
        flags |= FLAG_AGG_RETURN
    if key in REFUSED_SLOTS:
        flags |= FLAG_REFUSED
        plan["refuse"].append((-1, "%s::%s %s" % (key[0], key[1],
                                                  REFUSED_SLOTS[key])))

    for k, p in enumerate(params):
        stars, b = param_base(p)

        if stars == 0 and b in BYVAL_REFUSED:
            flags |= FLAG_REFUSED
            plan["refuse"].append(
                (k, "%s::%s arg %d (%s): %s" % (key[0], key[1], k, p,
                                                BYVAL_REFUSED[b])))
            continue
        if stars == 0 and b in BYVAL_AGGREGATES:
            flags |= FLAG_BYVAL_AGG
            plan["byval"].append((k, b))

        if b in struct_iface:
            flags |= FLAG_STRUCT_IFACE
            plan["struct"].append((k, b))

        if b in iface_names:
            if stars == 1:
                flags |= FLAG_IN_IFACE
                plan["in"].append(k)
            elif stars == 2:
                is_in = bool(re.search(r"\*\s*const\s*\*", p))
                s = spec.get(k)
                if s is None and has_count:
                    plan["refuse"].append(
                        (k, "%s::%s arg %d (%s) is array-capable and the method "
                            "declares a count argument, but ARRAY_SPECS has no "
                            "entry" % (key[0], key[1], k, p)))
                    flags |= FLAG_REFUSED
                    continue
                if s == "refuse":
                    plan["refuse"].append((k, "%s::%s arg %d refused by "
                                              "ARRAY_SPECS" % (key + (k,))))
                    flags |= FLAG_REFUSED
                    continue
                if s is None or s == "single":
                    if is_in:
                        plan["refuse"].append(
                            (k, "%s::%s arg %d is an IN array with no count"
                                % (key[0], key[1], k)))
                        flags |= FLAG_REFUSED
                        continue
                    flags |= FLAG_OUT_IFACE
                    plan["out"].append((k, b))
                else:
                    flags |= FLAG_IFACE_ARRAY
                    (plan["in_arr"] if is_in else plan["out_arr"]).append((k, s, b))
            else:
                plan["refuse"].append((k, "%s::%s arg %d has %d levels of "
                                          "indirection" % (key[0], key[1], k, stars)))
                flags |= FLAG_REFUSED
        elif b == "void" and stars == 2:
            if k in raw:
                flags |= FLAG_RAW_VOID
                plan["raw"].append(k)
                continue
            prev = params[k - 1] if k else None
            pb = param_base(prev)[1] if prev is not None else None
            if pb in ("REFIID", "REFGUID", "REFCLSID"):
                flags |= FLAG_VOID_OUT
                plan["riid"].append((k, k - 1))
            else:
                plan["refuse"].append(
                    (k, "%s::%s arg %d is a void** with no preceding REFIID and "
                        "is not in RAW_VOID_OUT" % (key[0], key[1], k)))
                flags |= FLAG_REFUSED
    return flags, plan


def plan_is_empty(plan):
    return not any(plan[k] for k in ("in", "out", "riid", "in_arr", "out_arr",
                                     "refuse", "struct"))


def check_invariants(ifaces, names, struct_iface):
    """Fail loudly rather than emit a table that is quietly wrong."""
    errs = []
    iface_names = set(names)

    for n in names:
        head = ifaces[n]["slots"][:3]
        if [s["owner"] for s in head] != ["IUnknown"] * 3 or \
           [s["name"] for s in head] != ["QueryInterface", "AddRef", "Release"]:
            errs.append("%s: slots 0/1/2 are not IUnknown QI/AddRef/Release" % n)

    # Every interface inheriting a float method must place it at the same slot,
    # or the shape-indexed slot table is a lie.
    fslot = {}
    fcount = {}
    for n in names:
        for s in ifaces[n]["slots"]:
            key = (s["owner"], s["name"])
            if key in FLOAT_METHODS:
                shape = FLOAT_METHODS[key][0]
                prev = fslot.setdefault(shape, (s["slot"], n))
                fcount[shape] = fcount.get(shape, 0) + 1
                if prev[0] != s["slot"]:
                    errs.append("%s: slot %d in %s but %d in %s"
                                % (shape, s["slot"], n, prev[0], prev[1]))
    for shape in FLOAT_SHAPE_ORDER:
        if shape not in fslot:
            errs.append("%s: no interface declares it" % shape)
    if sorted(FLOAT_SHAPE_ORDER) != sorted(v[0] for v in FLOAT_METHODS.values()):
        errs.append("FLOAT_SHAPE_ORDER and FLOAT_METHODS disagree")
    for shape, (_, ps) in FLOAT_PROTO.items():
        if shape not in FLOAT_SHAPE_ORDER:
            errs.append("FLOAT_PROTO has %s, which is not a shape" % shape)
        if sum(1 for _, _, kind in ps if kind == "f") > MAX_FLOAT_ARGS:
            errs.append("%s has more float args than MAX_FLOAT_ARGS" % shape)

    # FLOAT_METHODS completeness, in both directions.
    seen_float = set()
    for n in names:
        for s in ifaces[n]["slots"]:
            f = is_float_class(s["ret"])
            for p in s["params"]:
                stars, b = param_base(p)
                if stars == 0 and is_float_class(b):
                    f = True
            if f:
                seen_float.add((s["owner"], s["name"]))
    for k in sorted(seen_float):
        if k not in FLOAT_METHODS:
            errs.append("%s::%s takes or returns a float-class value and is not "
                        "in FLOAT_METHODS -- it would take the integer path and "
                        "read the wrong register" % k)
    for k in sorted(FLOAT_METHODS):
        if k not in seen_float:
            errs.append("%s::%s is in FLOAT_METHODS but has no float-class "
                        "parameter or return in interfaces.json" % k)
    # A float shape's prototype must match the census signature it stands for.
    for (owner, meth), (shape, _) in sorted(FLOAT_METHODS.items()):
        decl = None
        for n in names:
            for s in ifaces[n]["slots"]:
                if (s["owner"], s["name"]) == (owner, meth):
                    decl = s
                    break
            if decl:
                break
        proto = FLOAT_PROTO.get(shape)
        if decl is None or proto is None:
            continue
        if len(proto[1]) != len(decl["params"]):
            errs.append("%s: prototype has %d parameters, %s::%s has %d"
                        % (shape, len(proto[1]), owner, meth, len(decl["params"])))
            continue
        for i, (ctype, _, kind) in enumerate(proto[1]):
            stars, b = param_base(decl["params"][i])
            if kind == "f":
                if not (stars == 0 and is_float_class(b)):
                    errs.append("%s parameter %d is declared float but the "
                                "census says %r" % (shape, i, decl["params"][i]))
            elif kind == "i":
                if stars == 0 and (is_float_class(b) or b in BYVAL_AGGREGATES):
                    errs.append("%s parameter %d is declared integer-class but "
                                "the census says %r" % (shape, i,
                                                        decl["params"][i]))
            else:
                if not (stars == 0 and b == kind):
                    errs.append("%s parameter %d is declared %s but the census "
                                "says %r" % (shape, i, kind, decl["params"][i]))

    # The uint64_t[MAX_ARGS] convention: arity, and EVERY by-value type
    # accounted for.  Fail-closed: an unrecognised by-value type stops
    # generation rather than defaulting to the integer path.
    for n in names:
        for s in ifaces[n]["slots"]:
            if len(s["params"]) > MAX_ARGS:
                errs.append("%s::%s has %d parameters, MAX_ARGS is %d"
                            % (n, s["name"], len(s["params"]), MAX_ARGS))
            r = s["ret"]
            if r != "void" and not ret_is_pointer(r) and r not in BYVAL_INTEGER:
                if is_float_class(r):
                    errs.append("%s::%s returns %s, a float-class value, which "
                                "needs a hand-written shape" % (n, s["name"], r))
                else:
                    errs.append("%s::%s returns %s by value, which is not in "
                                "BYVAL_INTEGER. Classify it: an enum or an "
                                "<=8-byte integer aggregate is integer-class; "
                                "anything float-bearing is passed in FP "
                                "registers by BOTH ABIs." % (n, s["name"], r))
            if s.get("aggregate_return"):
                if not ret_is_pointer(r):
                    errs.append("%s::%s is flagged aggregate_return but returns "
                                "%s" % (n, s["name"], r))
                if not s["params"] or param_name(s["params"][0]) != "__ret":
                    errs.append("%s::%s is flagged aggregate_return but its "
                                "first parameter is not __ret" % (n, s["name"]))
            for p in s["params"]:
                stars, b = param_base(p)
                if stars or not b or b == "void":
                    continue
                if b in BYVAL_AGGREGATES or b in BYVAL_REFUSED:
                    continue
                if b not in BYVAL_INTEGER and not is_float_class(b):
                    errs.append("%s::%s takes %r by value, whose base type %s is "
                                "not in BYVAL_INTEGER. Classify it: enums and "
                                "<=8-byte integer aggregates are integer-class; "
                                "a float typedef or a float-bearing or >8-byte "
                                "aggregate is not, and would read an unrelated "
                                "register through the uint64_t transport."
                                % (n, s["name"], p, b))

    # ARRAY_SPECS / REFUSED_SLOTS / BYVAL_* / struct list: describe real things,
    # leave nothing unclassified, and contain no dead entries.
    used_arrays = set()
    used_raw = set()
    used_refused = set()
    used_byval = set()
    used_byval_refused = set()
    used_structs = set()
    for n in names:
        for s in ifaces[n]["slots"]:
            key = (s["owner"], s["name"])
            for p in s["params"]:
                stars, b = param_base(p)
                if stars == 0 and b in BYVAL_AGGREGATES:
                    used_byval.add(b)
                if stars == 0 and b in BYVAL_REFUSED:
                    used_byval_refused.add(b)
                if b in struct_iface:
                    used_structs.add(b)
            if key in ARRAY_SPECS:
                used_arrays.add(key)
            if key in RAW_VOID_OUT:
                used_raw.add(key)
            if key in REFUSED_SLOTS:
                used_refused.add(key)
            if hand_written(n, s):
                continue
            _, plan = classify(iface_names, struct_iface, n, s)
            for _, whyy in plan["refuse"]:
                if key not in REFUSED_SLOTS and not any(
                        param_base(p)[1] in BYVAL_REFUSED for p in s["params"]):
                    errs.append("unclassified: " + whyy)
            for k, j in plan["riid"]:
                if s["ret"] != "HRESULT":
                    errs.append("%s::%s has a riid-driven void** out but returns "
                                "%s, so the early E_NOINTERFACE path would "
                                "return the wrong type" % (n, s["name"], s["ret"]))
    for key in sorted(set(ARRAY_SPECS) - used_arrays):
        errs.append("ARRAY_SPECS has %s::%s, which no slot uses" % key)
    for key in sorted(set(RAW_VOID_OUT) - used_raw):
        errs.append("RAW_VOID_OUT has %s::%s, which no slot uses" % key)
    for key in sorted(set(REFUSED_SLOTS) - used_refused):
        errs.append("REFUSED_SLOTS has %s::%s, which no slot uses" % key)
    for b in sorted(set(BYVAL_REFUSED) - used_byval_refused):
        errs.append("BYVAL_REFUSED has %s, which no slot takes by value" % b)
    for b in sorted(set(OPAQUE_IFACE_STRUCTS) - used_structs):
        errs.append("OPAQUE_IFACE_STRUCTS has %s, which no slot takes" % b)
    # Every index in a used ARRAY_SPECS entry must name a real Iface** parameter.
    for n in names:
        for s in ifaces[n]["slots"]:
            key = (s["owner"], s["name"])
            for k in ARRAY_SPECS.get(key, {}):
                if k >= len(s["params"]):
                    errs.append("ARRAY_SPECS[%s::%s] index %d out of range"
                                % (key[0], key[1], k))
                    continue
                stars, b = param_base(s["params"][k])
                if stars != 2 or b not in iface_names:
                    errs.append("ARRAY_SPECS[%s::%s] index %d is %r, not an "
                                "interface array" % (key[0], key[1], k,
                                                     s["params"][k]))
            for k in RAW_VOID_OUT.get(key, set()):
                if k >= len(s["params"]) or param_base(s["params"][k]) != (2, "void"):
                    errs.append("RAW_VOID_OUT[%s::%s] index %d is not a void**"
                                % (key[0], key[1], k))

    # The pump overrides: derived slots, and their parameter shape, because the
    # host override indexes args[] positionally.
    pslot = {}
    pcount = {}
    for n in names:
        for s in ifaces[n]["slots"]:
            key = (s["owner"], s["name"])
            if key not in PUMP_SLOTS:
                continue
            kind, shape = PUMP_SLOTS[key]
            prev = pslot.setdefault(kind, (s["slot"], n))
            pcount[kind] = pcount.get(kind, 0) + 1
            if prev[0] != s["slot"]:
                errs.append("%s: slot %d in %s but %d in %s"
                            % (kind, s["slot"], n, prev[0], prev[1]))
            if len(s["params"]) != len(shape):
                errs.append("%s: %s::%s has %d parameters, the override expects "
                            "%d" % (kind, n, s["name"], len(s["params"]),
                                    len(shape)))
                continue
            for idx, want in shape:
                got = param_base(s["params"][idx])[1]
                if got != want:
                    errs.append("%s: %s::%s arg %d is %s, the override expects "
                                "%s" % (kind, n, s["name"], idx, got, want))
    for key in sorted(set(PUMP_SLOTS)):
        if PUMP_SLOTS[key][0] not in pslot:
            errs.append("PUMP_SLOTS has %s::%s, which no slot uses" % key)

    for n, m in NAMED_SLOTS:
        if n not in ifaces or not [s for s in ifaces[n]["slots"] if s["name"] == m]:
            errs.append("NAMED_SLOTS: %s::%s no longer exists; a test drives it"
                        % (n, m))

    uu = {}
    for n in names:
        u = ifaces[n].get("uuid")
        if not u:
            errs.append("%s: no uuid" % n)
        elif u in uu:
            errs.append("%s: uuid collides with %s" % (n, uu[u]))
        else:
            uu[u] = n

    if errs:
        for e in sorted(set(errs)):
            print("gen_thunk: " + e, file=sys.stderr)
        raise SystemExit("gen_thunk: invariant check failed")

    return ({k: v[0] for k, v in fslot.items()}, fcount,
            {k: v[0] for k, v in pslot.items()}, pcount)


# ---------------------------------------------------------------------------
def emit_ids(ifaces, out, fslot, pslot, struct_iface, iface_names):
    names = sorted(n for n, i in ifaces.items() if i["slots"])

    counts = {"in": 0, "out": 0, "arr": 0, "riid": 0, "raw": 0, "hand": 0,
              "clean": 0, "marshalled": 0, "refused": 0, "total": 0,
              "struct": 0, "agg": 0, "byval": 0, "pump": 0, "struct_only": 0}
    inventory = []          # (iface, slot, method, struct type)

    with open(os.path.join(out, "vkd3d_thunk_ids.h"), "w") as fh:
        fh.write(HEADER)
        fh.write("#pragma once\n#include <cstdint>\n"
                 '#include "vkd3d_thunk_abi.h"\n\n')

        fh.write("enum VkdIface : uint32_t {\n")
        for idx, n in enumerate(names):
            fh.write("    VKD3D_IFACE_%s = %d,\n" % (n.upper(), idx))
        fh.write("    VKD3D_IFACE_COUNT = %d\n};\n\n" % len(names))

        fh.write("""/* The uint64_t argument block every generated stub allocates.  The widest
   slot in the surface is 10 parameters, re-derived by check_invariants() from
   interfaces.json rather than trusted; VKD3D_THUNK_MAX_ARGS is the transport's
   ceiling. */
#define VKD3D_THUNK_ARGS %d
#define VKD3D_THUNK_FLOAT_ARGS %d
static_assert(VKD3D_THUNK_ARGS <= VKD3D_THUNK_MAX_ARGS,
              "argument block wider than the boundary contract allows");

/* HRESULTs, spelled out rather than pulled from a Windows header: neither half
   of this thunk includes one. */
#define VKD3D_S_OK           ((int32_t) 0x00000000)
#define VKD3D_S_FALSE        ((int32_t) 0x00000001)
#define VKD3D_E_NOTIMPL      ((int32_t) 0x80004001)
#define VKD3D_E_NOINTERFACE  ((int32_t) 0x80004002)
#define VKD3D_E_POINTER      ((int32_t) 0x80004003)
#define VKD3D_E_FAIL         ((int32_t) 0x80004005)
#define VKD3D_E_OUTOFMEMORY  ((int32_t) 0x8007000e)
#define VKD3D_E_INVALIDARG   ((int32_t) 0x80070057)

""" % (MAX_ARGS, MAX_FLOAT_ARGS))

        fh.write("""/* By-value aggregates of 8 bytes or less.  Transport is one uint64_t slot --
   MS-x64 passes any 1/2/4/8-byte aggregate in one GPR, SysV classifies one
   INTEGER eightbyte, ELFv2 passes it in a GPR -- but the ms_abi forwarders and
   the host-side float dispatch declare the REAL shape so that the compiler,
   not this generator, decides the parameter class.  The bit copy goes through
   a union rather than a pointer cast: reading one member of a union of equal
   size is the well-defined spelling, and it leaves no aliasing question. */
""")
        for census, (pod, member) in sorted(BYVAL_AGGREGATES.items()):
            fh.write("struct %s { %s };  /* %s */\n" % (pod, member, census))
        fh.write("""
template <typename T> static inline uint64_t vkd3d_agg_bits(T v) {
    union { T a; uint64_t u; } c;
    c.u = 0;
    c.a = v;
    return c.u;
}
template <typename T> static inline T vkd3d_agg_from(uint64_t u) {
    union { T a; uint64_t u; } c;
    c.u = u;
    return c.a;
}

""")

        fh.write("/* Slot counts, so the guest can size its vtables and the host\n"
                 "   can bounds-check an incoming slot index. */\n")
        fh.write("static const uint32_t kVkdSlotCount[VKD3D_IFACE_COUNT] = {\n")
        for n in names:
            fh.write("    %d, /* %s */\n" % (len(ifaces[n]["slots"]), n))
        fh.write("};\n\n")
        fh.write("static const char* const kVkdIfaceName[VKD3D_IFACE_COUNT] = {\n")
        for n in names:
            fh.write('    "%s",\n' % n)
        fh.write("};\n\n")

        # ---- IID table -------------------------------------------------
        fh.write("""/* IID -> interface id.  The two words are the GUID's 16-byte memory image
   read as little-endian 64-bit words -- the same bytes on x86-64 and on
   ppc64le, so a GUID written by the guest compares directly.  Sorted, for the
   binary search in vkd3d_iface_from_iid(). */
struct VkdIidEntry { uint64_t w0, w1; uint32_t iface; };

""")
        entries = []
        for n in names:
            b = uuid.UUID(ifaces[n]["uuid"]).bytes_le
            entries.append((int.from_bytes(b[0:8], "little"),
                            int.from_bytes(b[8:16], "little"), n))
        entries.sort()
        fh.write("static const uint32_t kVkdIidCount = %d;\n" % len(entries))
        fh.write("static const VkdIidEntry kVkdIids[] = {\n")
        for w0, w1, n in entries:
            fh.write("    { 0x%016xull, 0x%016xull, VKD3D_IFACE_%s }, /* %s */\n"
                     % (w0, w1, n.upper(), n))
        fh.write("};\n\n")

        # ---- float shapes ----------------------------------------------
        fh.write("""/* Float-class shapes.  The boundary call carries a SHAPE, not (iface, slot):
   the shape determines the native prototype, the slot only indexes the vtable.
   ClearDepthStencilView is slot %d of ID3D12GraphicsCommandList and of every
   version of it, so a switch on (iface, slot) would need one case per derived
   interface and would be the exact thing that rots. */
enum VkdFloatShape : uint32_t {
""" % fslot["VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW"])
        for i, shape in enumerate(FLOAT_SHAPE_ORDER):
            ret, ps = FLOAT_PROTO[shape]
            fh.write("    /* %s (this%s) */\n"
                     % (ret, "".join(", " + t for t, _, _ in ps)))
            fh.write("    %s = %d,\n" % (shape, i))
        fh.write("    VKD3D_FSHAPE_COUNT = %d\n};\n\n" % len(FLOAT_SHAPE_ORDER))
        fh.write("static const uint32_t kVkdFloatSlot[VKD3D_FSHAPE_COUNT] = {\n")
        for shape in FLOAT_SHAPE_ORDER:
            fh.write("    %d, /* %s */\n" % (fslot[shape], shape))
        fh.write("};\n\n")

        # ---- pump overrides --------------------------------------------
        fh.write("""/* Host-side slot overrides: the fence/event pump.  These are the ONLY entries
   in an otherwise fully generic dispatcher, and the (iface, slot) pairs are
   derived from the census -- SetEventOnCompletion is slot %d of every fence
   interface, SetEventOnMultipleFenceCompletion slot %d of every ID3D12Device1+
   -- so no slot number is written down by hand anywhere. */
enum VkdPumpKind : uint32_t {
""" % (pslot["VKD3D_PUMP_FENCE_EVENT"], pslot["VKD3D_PUMP_MULTI_FENCE_EVENT"]))
        for i, k in enumerate(PUMP_KIND_ORDER):
            fh.write("    %s = %d,\n" % (k, i))
        fh.write("    VKD3D_PUMP_KIND_COUNT = %d\n};\n\n" % len(PUMP_KIND_ORDER))

        pump_rows = []
        for idx, n in enumerate(names):
            for s in ifaces[n]["slots"]:
                key = (s["owner"], s["name"])
                if key in PUMP_SLOTS:
                    pump_rows.append((idx, s["slot"], PUMP_SLOTS[key][0], n,
                                      s["name"]))
        pump_rows.sort()
        fh.write("struct VkdPumpSlot { uint32_t iface, slot, kind; };\n")
        fh.write("static const uint32_t kVkdPumpSlotCount = %d;\n" % len(pump_rows))
        fh.write("static const VkdPumpSlot kVkdPumpSlots[] = {\n")
        for i, sl, kind, n, m in pump_rows:
            fh.write("    { VKD3D_IFACE_%s, %d, %s }, /* %s::%s */\n"
                     % (n.upper(), sl, kind, n, m))
        fh.write("};\n\n")
        fh.write("""static inline uint32_t vkd3d_pump_kind(uint32_t iface, uint32_t slot) {
    for (uint32_t i = 0; i < kVkdPumpSlotCount; i++)
        if (kVkdPumpSlots[i].iface == iface && kVkdPumpSlots[i].slot == slot)
            return kVkdPumpSlots[i].kind;
    return VKD3D_PUMP_NONE;
}

""")

        # ---- per-slot flags --------------------------------------------
        fh.write("""/* Per-slot record of what a slot's parameters need AND whether the generated
   stub handles it.  MARSHALLED means the guest stub wraps/unwraps every
   interface pointer in the signature; HAND means a runtime/ symbol does.
   VKD3D_THUNK_STRICT=1 makes the host dispatcher warn the first time a slot is
   called that is neither. */
#define VKD3D_SLOT_IN_IFACE     1u
#define VKD3D_SLOT_OUT_IFACE    2u
#define VKD3D_SLOT_IFACE_ARRAY  4u
#define VKD3D_SLOT_VOID_OUT     8u    /* riid-driven void** out */
#define VKD3D_SLOT_HAND         16u
#define VKD3D_SLOT_MARSHALLED   32u
#define VKD3D_SLOT_RAW_VOID     64u   /* void** that is memory, not an interface */
#define VKD3D_SLOT_REFUSED      128u
#define VKD3D_SLOT_STRUCT_IFACE 256u  /* by-pointer struct with interface members */
#define VKD3D_SLOT_AGG_RETURN   512u  /* explicit __ret pointer, returned back */
#define VKD3D_SLOT_BYVAL_AGG    1024u /* by-value aggregate of <= 8 bytes */
#define VKD3D_SLOT_PUMP         2048u /* host-side override: the fence/event pump */

/* Bits that mean "this slot carries an interface pointer". */
#define VKD3D_SLOT_IFACE_PTRS \\
    (VKD3D_SLOT_IN_IFACE | VKD3D_SLOT_OUT_IFACE | VKD3D_SLOT_IFACE_ARRAY | \\
     VKD3D_SLOT_VOID_OUT)
/* Bits that mean "and something translates them". */
#define VKD3D_SLOT_HANDLED    (VKD3D_SLOT_HAND | VKD3D_SLOT_MARSHALLED)

/* THE definition of "this slot still hands a pointer across untranslated".
   One expression, used by the STRICT dispatcher and by tests/build.sh, so the
   two cannot disagree.

   Note the shape: it is NOT a bit mask.  The obvious `f & NEEDS_WORK &
   ~HANDLED` reads like one and is a no-op, because NEEDS_WORK and HANDLED are
   disjoint sets -- that is defect A3 in the D3D11 project, and it is worth not
   repeating.

   RAW_VOID is deliberately NOT reported, which is a divergence from the D3D11
   reference.  Its one raw void** is ID3D12Resource::Map's `void **data`: a
   pointer to mapped device memory that the guest writes through directly.
   That is correct under the shared address space FEX gives us -- the same
   property every OUT parameter here already depends on -- so warning about it
   would be noise, not a finding.  dxvk reports its equivalent because the
   correctness of GetDecoderBuffer's buffer had never been established there.
   STRUCT_IFACE is reported: the struct passes by pointer with no repacking, so
   the interface pointers INSIDE it are still guest proxies.  REFUSED means the
   generator could not marshal the slot at all. */
static inline int vkd3d_slot_untranslated(uint16_t f) {
    if ((f & VKD3D_SLOT_IFACE_PTRS) && !(f & VKD3D_SLOT_HANDLED))
        return 1;
    return (f & (VKD3D_SLOT_REFUSED | VKD3D_SLOT_STRUCT_IFACE)) ? 1 : 0;
}

""")
        for n in names:
            flags = []
            for s in ifaces[n]["slots"]:
                counts["total"] += 1
                f, plan = classify(iface_names, struct_iface, n, s)
                hand = hand_written(n, s)
                if hand:
                    # The float stubs unwrap nothing (no interface parameters in
                    # any of the three shapes) but record what they carry.
                    f = (f & (FLAG_AGG_RETURN | FLAG_BYVAL_AGG)) | FLAG_HAND
                    counts["hand"] += 1
                else:
                    if f & FLAG_AGG_RETURN:
                        counts["agg"] += 1
                    if f & FLAG_BYVAL_AGG:
                        counts["byval"] += 1
                    if f & FLAG_RAW_VOID:
                        counts["raw"] += 1
                    if f & FLAG_STRUCT_IFACE:
                        counts["struct"] += 1
                        for _, b in plan["struct"]:
                            inventory.append((n, s["slot"], s["name"], b))
                    if f & FLAG_REFUSED:
                        counts["refused"] += 1
                    elif f & (FLAG_IN_IFACE | FLAG_OUT_IFACE | FLAG_IFACE_ARRAY |
                              FLAG_VOID_OUT):
                        f |= FLAG_MARSHALLED
                        counts["marshalled"] += 1
                        if f & FLAG_IN_IFACE:
                            counts["in"] += 1
                        if f & FLAG_OUT_IFACE:
                            counts["out"] += 1
                        if f & FLAG_IFACE_ARRAY:
                            counts["arr"] += 1
                        if f & FLAG_VOID_OUT:
                            counts["riid"] += 1
                    elif f & FLAG_STRUCT_IFACE:
                        counts["struct_only"] += 1
                    else:
                        counts["clean"] += 1
                if (s["owner"], s["name"]) in PUMP_SLOTS:
                    f |= FLAG_PUMP
                    counts["pump"] += 1
                flags.append(f)
            fh.write("static const uint16_t kVkdSlotFlags_%s[] = {" % n)
            fh.write(",".join(str(f) for f in flags))
            fh.write("};\n")
        fh.write("\nstatic const uint16_t* const kVkdSlotFlags[VKD3D_IFACE_COUNT] = {\n")
        for n in names:
            fh.write("    kVkdSlotFlags_%s,\n" % n)
        fh.write("};\n")

        # ---- host-side prototypes --------------------------------------
        fh.write("""
/* The host half of the boundary, restated as declarations.  These four names
   and their flattened-scalar signatures are fixed by runtime/vkd3d_thunk_abi.h
   (see "host-side exports (thunkgen surface)" there, which spells them out in
   prose); they live here rather than in that file so the boundary contract
   itself stays untouched by this generator. */
extern "C" {
uint64_t vkd3d_host_dispatch(uint32_t iface, uint32_t slot, uint64_t host,
                             uint64_t* args);
uint64_t vkd3d_host_dispatch_float(uint32_t iface, uint32_t slot, uint32_t shape,
                                   uint64_t host, uint64_t* args,
                                   const float* fin, float* fout);
uint32_t vkd3d_host_entry(uint32_t entry, uint64_t* args);
uint32_t vkd3d_host_probe(void);

/* The pump overrides, implemented in runtime/vkd3d_thunk_host_rt.cpp and
   reached from the generated dispatcher. */
uint64_t vkd3d_host_pump_dispatch(uint32_t kind, uint32_t iface, uint32_t slot,
                                  uint64_t host, uint64_t* args);
}

""")

        # ---- named slots for the tests ---------------------------------
        fh.write("/* Slot numbers the tests drive by name, so a regenerated\n"
                 "   interfaces.json cannot leave a test calling the neighbouring\n"
                 "   method with this method's arguments. */\n")
        for n, m in NAMED_SLOTS:
            got = [s["slot"] for s in ifaces[n]["slots"] if s["name"] == m]
            fh.write("#define VKD3D_SLOT_%s_%s %du\n" % (n.upper(), m.upper(), got[0]))

    return names, counts, inventory


# ---------------------------------------------------------------------------
def emit_host(ifaces, names, out):
    """Host side: resolve (iface, slot) to a native vtable entry and call it."""
    path = os.path.join(out, "vkd3d_thunk_host.cpp")
    with open(path, "w") as fh:
        fh.write(HEADER)
        fh.write("""#include "vkd3d_thunk_ids.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Every D3D12 argument is integer-class and fits in 64 bits -- pointers,
 * scalars, enums, and the two 8-byte descriptor-handle aggregates that all
 * three ABIs pass in a single GPR -- so one signature covers the whole surface
 * except the float-class shapes, which go through vkd3d_host_dispatch_float in
 * runtime/vkd3d_thunk_host_rt.cpp.
 *
 * Aggregate-return slots ride this same prototype: the widl C vtable gives them
 * an explicit `RET *__ret` first parameter and returns that pointer, so they
 * are (this, retptr, args...) -> retptr at every ABI level. */
typedef uint64_t (*GenericFn)(void*, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t);

/* VKD3D_THUNK_STRICT=1: warn once per slot that carries an interface pointer
 * which is NOT translated by either a generated stub or a hand-written runtime
 * symbol.  The predicate is vkd3d_slot_untranslated(), generated in
 * vkd3d_thunk_ids.h and shared with tests/build.sh, so there is exactly one
 * definition of "untranslated". */
static bool vkd3d_thunk_strict() {
    static const bool on = [] {
        const char* e = std::getenv("VKD3D_THUNK_STRICT");
        return e && e[0] == '1';
    }();
    return on;
}

extern "C" uint64_t vkd3d_host_dispatch(uint32_t iface, uint32_t slot,
                                        uint64_t host, uint64_t* a) {
    if (iface >= VKD3D_IFACE_COUNT || slot >= kVkdSlotCount[iface] || !host || !a) {
        std::fprintf(stderr, "vkd3d_thunk: bad dispatch iface=%u slot=%u host=%p\\n",
                     iface, slot, (void*) host);
        return 0;
    }

    const uint16_t f = kVkdSlotFlags[iface][slot];

    /* The only non-generic entries in the dispatcher: the fence/event pump.
     * (iface, slot) pairs come from the generated table, so adding
     * ID3D12Device16 to the census wires its SetEventOnMultipleFenceCompletion
     * automatically rather than silently leaving it unpumped. */
    if (f & VKD3D_SLOT_PUMP) {
        uint32_t kind = vkd3d_pump_kind(iface, slot);
        if (kind != VKD3D_PUMP_NONE)
            return vkd3d_host_pump_dispatch(kind, iface, slot, host, a);
    }

    if (vkd3d_thunk_strict()) {
        static uint8_t warned[VKD3D_IFACE_COUNT][256] = {};
        if (vkd3d_slot_untranslated(f) && slot < 256 && !warned[iface][slot]) {
            warned[iface][slot] = 1;
            std::fprintf(stderr, "vkd3d_thunk: STRICT %s slot %u passes "
                         "untranslated pointers (flags 0x%x)\\n",
                         kVkdIfaceName[iface], slot, f);
        }
    }

    /* A COM object's first word is its vtable pointer; slot indexes it.  This
     * is the whole reason the slot table has to be generated from the headers
     * rather than written by hand -- an off-by-one here calls the neighbouring
     * method with this method's arguments, and nothing diagnoses it. */
    void*  obj  = reinterpret_cast<void*>(host);
    void** vtbl = *reinterpret_cast<void***>(obj);
    GenericFn fn = reinterpret_cast<GenericFn>(vtbl[slot]);

    return fn(obj, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
}
""")
    return path


GUEST_PROLOGUE = """#include "vkd3d_thunk_ids.h"
#include "vkd3d_proxy.h"
#include "vkd3d_thunk_abi.h"

#include <cstdint>
#include <cstring>

/* Slots 0/1/2 of every vtable below are QueryInterface / AddRef / Release,
 * served by runtime/vkd3d_proxy.cpp rather than by a generated stub: interning
 * and reference counting are policy, not argument packing.  Likewise the three
 * float-class shapes, whose arguments the ABI puts in FP registers.
 * Declarations for both are in vkd3d_proxy.h. */

/* Stubs and vtables must survive optimisation.  Declaring them `static` makes
 * the entire graph unreferenced -- at -O0 all stubs are present, at -O2 none
 * are.  They get external linkage with hidden visibility (kept by the linker,
 * not exported from the library) and are gathered into a registry below that is
 * itself referenced, so nothing can be proven dead. */
#define VKD3D_THUNK_STUB __attribute__((visibility("hidden"), used))
#define VKD3D_THUNK_VTBL __attribute__((visibility("hidden"), used))

/* ---------------------------------------------------------------------------
 * Calling convention.
 *
 * The worker stubs below are SysV: they are what a guest ELF caller invokes
 * directly.  A PE game calls with MS-x64, so on x86-64 every slot also gets an
 * ms_abi forwarder that calls through the per-interface TARGET table.  The
 * indirection is what lets a later package override a slot (the hand-written
 * struct fixups are the next one) for PE callers only, with a plain SysV
 * function: the convention has already been converted by the time the override
 * is reached.
 *
 * VKD3D_THUNK_ABI_NEGATIVE_CONTROL deliberately compiles the forwarders as
 * SysV.  tests/build.sh builds the MS-x64-caller test that way once and
 * requires it to FAIL, which is the evidence that the test can see this class
 * of defect at all.
 * ------------------------------------------------------------------------- */
#if defined(__x86_64__)
#  define VKD3D_HAVE_MS_ABI 1
#  if defined(VKD3D_THUNK_ABI_NEGATIVE_CONTROL)
#    define VKD3D_MS_ABI /* deliberately wrong; falsification build only */
#  else
#    define VKD3D_MS_ABI __attribute__((ms_abi))
#  endif
#else
#  define VKD3D_HAVE_MS_ABI 0
#  define VKD3D_MS_ABI
#endif

"""


def emit_guest(ifaces, names, out, struct_iface):
    """Guest side: three arrays per interface type, not per object."""
    path = os.path.join(out, "vkd3d_thunk_guest.cpp")
    iface_names = set(names)
    stubs = 0
    fwd = 0
    with open(path, "w") as fh:
        fh.write(HEADER)
        fh.write(GUEST_PROLOGUE)

        # SysV function-pointer types the ms_abi forwarders call through.  No
        # ABI attribute, i.e. SysV, which is what the target table holds.
        for k in range(MAX_ARGS + 1):
            args = "".join(", uint64_t" for _ in range(k))
            fh.write("typedef uint64_t (*VkdSysvFn%d)(Proxy*%s);\n" % (k, args))
        fh.write("typedef int32_t  (*VkdSysvQiFn)(Proxy*, const void*, void**);\n")
        fh.write("typedef uint32_t (*VkdSysvRefFn)(Proxy*);\n")
        for shape in FLOAT_SHAPE_ORDER:
            ret, ps = FLOAT_PROTO[shape]
            tys = "".join(", " + t for t, _, _ in ps)
            fh.write("typedef %s (*VkdSysvF_%s)(Proxy*%s);\n" % (ret, shape, tys))
        fh.write("\n")

        for n in names:
            slots = ifaces[n]["slots"]
            fh.write("/* ---- %s: %d slots ---- */\n" % (n, len(slots)))

            for s in slots:
                if hand_written(n, s):
                    continue
                stubs += 1
                fh.write(emit_worker(n, s, iface_names, struct_iface))

            fh.write("\nVKD3D_THUNK_VTBL const void* k%s_target[%d] = {\n"
                     % (n, len(slots)))
            for s in slots:
                hand = hand_written(n, s)
                sym = hand if hand else "%s_%d_%s" % (n, s["slot"], s["name"])
                fh.write("    (const void*) %s,\n" % sym)
            fh.write("};\n")

            fh.write("VKD3D_THUNK_VTBL const void* k%s_vtbl_sysv[%d] = {\n"
                     % (n, len(slots)))
            for s in slots:
                hand = hand_written(n, s)
                sym = hand if hand else "%s_%d_%s" % (n, s["slot"], s["name"])
                fh.write("    (const void*) %s,\n" % sym)
            fh.write("};\n")

            fh.write("#if VKD3D_HAVE_MS_ABI\n")
            for s in slots:
                fwd += 1
                fh.write(emit_forwarder(n, s))
            fh.write("VKD3D_THUNK_VTBL const void* k%s_vtbl_ms[%d] = {\n"
                     % (n, len(slots)))
            for s in slots:
                fh.write("    (const void*) %s_%d_ms,\n" % (n, s["slot"]))
            fh.write("};\n")
            fh.write("#endif\n\n")

        fh.write("""/* iface id -> vtable.  Referenced, so nothing above is dead.

   vkd3d_thunk_vtable() is the PE-FACING TARGET TABLE: the array the ms_abi
   forwarders read, and the one a slot override must patch.  Proxies get their
   vtable from vkd3d_thunk_vtable_for(), which picks the SysV or the MS array
   according to the process-global guest ABI mode. */
""")
        fh.write('extern "C" const void* const* vkd3d_thunk_vtable(uint32_t iface) {\n')
        fh.write("    static const void* const* const kTargets[] = {\n")
        for n in names:
            fh.write("        k%s_target,\n" % n)
        fh.write("    };\n")
        fh.write("    return iface < VKD3D_IFACE_COUNT ? kTargets[iface] : nullptr;\n}\n\n")

        fh.write('extern "C" const void* const* vkd3d_thunk_vtable_for(uint32_t iface,\n'
                 '                                                     uint32_t abi) {\n')
        fh.write("    static const void* const* const kSysv[] = {\n")
        for n in names:
            fh.write("        k%s_vtbl_sysv,\n" % n)
        fh.write("    };\n")
        fh.write("#if VKD3D_HAVE_MS_ABI\n")
        fh.write("    static const void* const* const kMs[] = {\n")
        for n in names:
            fh.write("        k%s_vtbl_ms,\n" % n)
        fh.write("    };\n")
        fh.write("#endif\n")
        fh.write("    if (iface >= VKD3D_IFACE_COUNT)\n        return nullptr;\n")
        fh.write("#if VKD3D_HAVE_MS_ABI\n")
        fh.write("    if (abi == VKD3D_ABI_MS)\n        return kMs[iface];\n")
        fh.write("#endif\n")
        fh.write("    return abi == VKD3D_ABI_SYSV ? kSysv[iface] : nullptr;\n}\n\n")

        fh.write('extern "C" uint32_t vkd3d_thunk_abi_available(void) {\n')
        fh.write("#if VKD3D_HAVE_MS_ABI\n    return (1u << VKD3D_ABI_SYSV) | (1u << VKD3D_ABI_MS);\n")
        fh.write("#else\n    return 1u << VKD3D_ABI_SYSV;\n#endif\n}\n")
    return path, stubs, fwd


def count_expr(spec, guard=None):
    kind, j = spec
    if kind == "arg":
        e = "(uint32_t) a%d" % j
    else:
        e = "(a%d ? *(const uint32_t*)(uintptr_t) a%d : 0u)" % (j, j)
    if guard is not None:
        e = "(%s ? %s : 0u)" % (guard, e)
    return e


def poison_return(s):
    """What a refused stub returns instead of crossing."""
    if s["ret"] == "HRESULT":
        return "(uint64_t)(uint32_t) VKD3D_E_NOTIMPL"
    return "0"


def emit_worker(n, s, iface_names, struct_iface):
    """One SysV worker stub, with its interface-pointer marshalling."""
    up = n.upper()
    slot = s["slot"]
    argc = len(s["params"])
    flags, plan = classify(iface_names, struct_iface, n, s)
    sig = "".join(", uint64_t a%d" % k for k in range(argc))
    o = ["VKD3D_THUNK_STUB uint64_t %s_%d_%s(Proxy* self%s) {\n"
         % (n, slot, s["name"], sig)]

    if plan["refuse"]:
        why = plan["refuse"][0][1].replace('"', "'")
        o.append('    vkd3d_thunk_refuse("%s::%s", "%s");\n' % (n, s["name"], why))
        o.append("    return %s;\n}\n" % poison_return(s))
        return "".join(o)

    pre = []
    if plan["struct"]:
        # Crosses RAW, and says so on every call.  The struct passes by pointer
        # with no repacking, so the interface pointers inside it arrive at
        # native vkd3d as guest Proxy*.  Hand-written fixups are the next work
        # package; until then this is loud rather than silent, and
        # VKD3D_THUNK_STRICT=1 turns it into an abort.
        for k, b in plan["struct"]:
            pre.append('    vkd3d_thunk_struct_iface("%s::%s", "%s", %d);\n'
                       % (n, s["name"], b, k))

    if plan_is_empty(plan):
        o.extend(pre)
        o.append("    uint64_t a[VKD3D_THUNK_ARGS] = {%s};\n"
                 % ", ".join("a%d" % k for k in range(argc)))
        o.append("    return vkd3d_thunk_call(VKD3D_IFACE_%s, %d, self->host, a);\n}\n"
                 % (up, slot))
        return "".join(o)

    o.extend(pre)
    expr = ["a%d" % k for k in range(argc)]
    post = []

    # riid-driven void** outs come first: an unknown IID is answered without
    # crossing, exactly as QueryInterface does, because we would have no vtable
    # to hand back anyway.
    for k, j in plan["riid"]:
        o.append("    uint32_t w%d = vkd3d_iface_from_iid((const void*)(uintptr_t) a%d);\n"
                 % (k, j))
        o.append("    if (w%d == VKD3D_IFACE_INVALID) {\n" % k)
        o.append("        if (a%d) *(void**)(uintptr_t) a%d = nullptr;\n" % (k, k))
        o.append("        return (uint64_t)(uint32_t) VKD3D_E_NOINTERFACE;\n    }\n")
        o.append("    uint64_t h%d = 0;\n" % k)
        expr[k] = "(a%d ? (uint64_t)(uintptr_t) &h%d : 0)" % (k, k)
        post.append("    if (a%d) *(void**)(uintptr_t) a%d = vkd3d_proxy_wrap(h%d, w%d);\n"
                    % (k, k, k, k))

    for k in plan["in"]:
        expr[k] = "vkd3d_proxy_unwrap((void*)(uintptr_t) a%d)" % k

    for k, b in plan["out"]:
        o.append("    uint64_t h%d = 0;\n" % k)
        expr[k] = "(a%d ? (uint64_t)(uintptr_t) &h%d : 0)" % (k, k)
        post.append("    if (a%d) *(void**)(uintptr_t) a%d = vkd3d_proxy_wrap(h%d, %s);\n"
                    % (k, k, k, iface_enum(b)))

    for k, spec, b in plan["in_arr"]:
        o.append("    VkdIfArray s%d;\n" % k)
        o.append("    uint64_t p%d = vkd3d_ifarray_in(&s%d, (void* const*)(uintptr_t) a%d, %s);\n"
                 % (k, k, k, count_expr(spec, "a%d" % k)))
        expr[k] = "p%d" % k
        post.append("    vkd3d_ifarray_free(&s%d);\n" % k)

    for k, spec, b in plan["out_arr"]:
        o.append("    uint32_t n%d = %s;\n" % (k, count_expr(spec, "a%d" % k)))
        o.append("    VkdIfArray s%d;\n" % k)
        o.append("    uint64_t p%d = vkd3d_ifarray_out(&s%d, n%d);\n" % (k, k, k))
        expr[k] = "p%d" % k
        post.append("    vkd3d_ifarray_wrap_out(&s%d, (void**)(uintptr_t) a%d, n%d, %s);\n"
                    % (k, k, k, iface_enum(b)))
        post.append("    vkd3d_ifarray_free(&s%d);\n" % k)

    o.append("    uint64_t a[VKD3D_THUNK_ARGS] = {%s};\n" % ", ".join(expr))
    o.append("    uint64_t r_ = vkd3d_thunk_call(VKD3D_IFACE_%s, %d, self->host, a);\n"
             % (up, slot))
    o.extend(post)
    o.append("    return r_;\n}\n")
    return "".join(o)


def ms_param(k, p, s):
    """(declared type, expression that yields the uint64_t transport slot).

    By-value aggregates are declared with their REAL shape so MS-x64 places
    them itself, then bit-copied into the transport slot.  Everything else is
    already integer-class and rides uint64_t unchanged; an aggregate-return
    slot's __ret is spelled as a pointer, which is what it is. """
    stars, b = param_base(p)
    if k == 0 and s.get("aggregate_return"):
        return "void*", "(uint64_t)(uintptr_t) a0"
    if stars == 0 and b in BYVAL_AGGREGATES:
        return BYVAL_AGGREGATES[b][0], "vkd3d_agg_bits(a%d)" % k
    return "uint64_t", "a%d" % k


def emit_forwarder(n, s):
    """One ms_abi forwarder: convert the convention, then call the target."""
    slot = s["slot"]
    name = "%s_%d_ms" % (n, slot)
    tgt = "k%s_target[%d]" % (n, slot)
    key = (s["owner"], s["name"])

    if slot == 0:
        return ("VKD3D_THUNK_STUB VKD3D_MS_ABI int32_t %s(Proxy* self, "
                "const void* riid, void** ppv) {\n"
                "    return ((VkdSysvQiFn) %s)(self, riid, ppv);\n}\n" % (name, tgt))
    if slot in (1, 2):
        return ("VKD3D_THUNK_STUB VKD3D_MS_ABI uint32_t %s(Proxy* self) {\n"
                "    return ((VkdSysvRefFn) %s)(self);\n}\n" % (name, tgt))
    if key in FLOAT_METHODS:
        shape = FLOAT_METHODS[key][0]
        ret, ps = FLOAT_PROTO[shape]
        sig = ", ".join("%s %s" % (t, v) for t, v, _ in ps)
        call = ", ".join(v for _, v, _ in ps)
        r = "" if ret == "void" else "return "
        return ("VKD3D_THUNK_STUB VKD3D_MS_ABI %s %s(Proxy* self, %s) {\n"
                "    %s((VkdSysvF_%s) %s)(self, %s);\n}\n"
                % (ret, name, sig, r, shape, tgt, call))

    argc = len(s["params"])
    decls = []
    args = []
    for k, p in enumerate(s["params"]):
        ty, ex = ms_param(k, p, s)
        decls.append(", %s a%d" % (ty, k))
        args.append(", " + ex)
    if s.get("aggregate_return"):
        # `RET* (ms_abi)(Proxy*, RET* __ret, args...)`, spelled with void*
        # because the guest half has no d3d12 headers and every pointer is one
        # pointer.  The real return type is in the comment.
        return ("VKD3D_THUNK_STUB VKD3D_MS_ABI void* %s(Proxy* self%s) {\n"
                "    /* aggregate return: %s */\n"
                "    return (void*)(uintptr_t) ((VkdSysvFn%d) %s)(self%s);\n}\n"
                % (name, "".join(decls), s["ret"], argc, tgt, "".join(args)))
    return ("VKD3D_THUNK_STUB VKD3D_MS_ABI uint64_t %s(Proxy* self%s) {\n"
            "    return ((VkdSysvFn%d) %s)(self%s);\n}\n"
            % (name, "".join(decls), argc, tgt, "".join(args)))


def emit_counts(out, names, total, stubs, fwd, counts, fcount, inventory):
    """Numbers tests/build.sh checks against, so expectations cannot rot."""
    path = os.path.join(out, "vkd3d_thunk_counts.sh")
    with open(path, "w") as fh:
        fh.write("# GENERATED by gen_thunk.py -- sourced by tests/build.sh\n")
        fh.write("VKD3D_N_IFACES=%d\n" % len(names))
        fh.write("VKD3D_N_SLOTS=%d\n" % total)
        fh.write("VKD3D_N_WORKERS=%d\n" % stubs)
        fh.write("VKD3D_N_FORWARDERS=%d\n" % fwd)
        fh.write("VKD3D_N_MARSHALLED=%d\n" % counts["marshalled"])
        fh.write("VKD3D_N_HAND=%d\n" % counts["hand"])
        fh.write("VKD3D_N_CLEAN=%d\n" % counts["clean"])
        fh.write("VKD3D_N_RAWVOID=%d\n" % counts["raw"])
        fh.write("VKD3D_N_STRUCTIFACE=%d\n" % counts["struct"])
        fh.write("VKD3D_N_REFUSED=%d\n" % counts["refused"])
        fh.write("VKD3D_N_AGGRET=%d\n" % counts["agg"])
        fh.write("VKD3D_N_BYVALAGG=%d\n" % counts["byval"])
        fh.write("VKD3D_N_PUMP=%d\n" % counts["pump"])
        fh.write("VKD3D_N_FLOATSLOTS=%d\n" % sum(fcount.values()))
        fh.write("VKD3D_N_FLOATSHAPES=%d\n" % len(FLOAT_SHAPE_ORDER))
        fh.write("VKD3D_N_STRUCTIFACE_ENTRIES=%d\n" % len(inventory))
    return path


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--json", default=os.path.join(here, "interfaces.json"))
    ap.add_argument("--idl-gen", default=os.path.join(os.path.dirname(here),
                                                      "idl", "gen"))
    ap.add_argument("--out", default="generated")
    args = ap.parse_args()

    ifaces = load(args.json)
    os.makedirs(args.out, exist_ok=True)

    names = sorted(n for n, i in ifaces.items() if i["slots"])
    iface_names = set(names)

    struct_iface, n_aggs = iface_bearing_structs(args.idl_gen, iface_names)
    fslot, fcount, pslot, pcount = check_invariants(ifaces, names, struct_iface)

    names, counts, inventory = emit_ids(ifaces, args.out, fslot, pslot,
                                        struct_iface, iface_names)
    host = emit_host(ifaces, names, args.out)
    guest, stubs, fwd = emit_guest(ifaces, names, args.out, struct_iface)

    total = sum(len(ifaces[n]["slots"]) for n in names)
    hand_float = counts["hand"] - 3 * len(names)
    cnt = emit_counts(args.out, names, total, stubs, fwd, counts, fcount,
                      inventory)

    print("interfaces           : %d" % len(names))
    print("vtable slots         : %d" % total)
    print("  SysV worker stubs  : %d" % stubs)
    print("  ms_abi forwarders  : %d (x86-64 only)" % fwd)
    print("  hand-written       : %d (%d IUnknown + %d float-class)"
          % (counts["hand"], 3 * len(names), hand_float))
    print("  float-class shapes : %d across %d vtable slots"
          % (len(FLOAT_SHAPE_ORDER), sum(fcount.values())))
    for shape in FLOAT_SHAPE_ORDER:
        print("      %-38s slot %-3d x%d" % (shape, fslot[shape], fcount[shape]))
    print("D3D12-specific shapes:")
    print("  aggregate-return   : %d slots (explicit __ret, generic path)"
          % counts["agg"])
    print("  by-value aggregate : %d slots (<= 8 bytes, one uint64 slot)"
          % counts["byval"])
    print("interface-pointer marshalling, generated per slot:")
    print("  slots marshalled   : %d" % counts["marshalled"])
    print("    IN  Iface*       : %d" % counts["in"])
    print("    OUT Iface**      : %d" % counts["out"])
    print("    Iface array+count: %d" % counts["arr"])
    print("    riid void** out  : %d" % counts["riid"])
    print("  raw void** (memory): %d (ID3D12Resource::Map; passed through)"
          % counts["raw"])
    print("  struct with iface  : %d slots, %d (slot, struct) pairs, %d struct "
          "types" % (counts["struct"], len(inventory),
                     len({b for _, _, _, b in inventory})))
    print("  refused            : %d" % counts["refused"])
    print("  struct-iface only  : %d (no other pointer in the signature)"
          % counts["struct_only"])
    print("  no pointers at all : %d" % counts["clean"])
    print("  reconciliation     : %d + %d + %d + %d + %d = %d"
          % (counts["marshalled"], counts["hand"], counts["refused"],
             counts["struct_only"], counts["clean"], total))
    print("host slot overrides  : %d (fence/event pump)" % counts["pump"])
    for kind in PUMP_KIND_ORDER[1:]:
        print("      %-30s slot %-3d x%d" % (kind, pslot[kind], pcount[kind]))
    print("struct-with-interface inventory (%d aggregates scanned in %s):"
          % (n_aggs, os.path.relpath(args.idl_gen)))
    for iface, slot, meth, b in sorted(inventory):
        print("      %-28s slot %-3d %-28s %s" % (iface, slot, meth, b))
    print("\nwrote %s" % os.path.join(args.out, "vkd3d_thunk_ids.h"))
    print("wrote %s" % host)
    print("wrote %s" % guest)
    print("wrote %s" % cnt)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GenError as exc:
        sys.stderr.write("gen_thunk: %s\n" % exc)
        sys.exit(1)
