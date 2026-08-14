#!/usr/bin/env python3
"""Census every COM interface in the widl-generated D3D12 C headers.

Reads the machine-generated headers under ppc64le/idl/gen and emits
ppc64le/thunk/interfaces.json: for each interface its IID, its direct base,
the header it lives in, and its complete vtable in true slot order (inherited
methods included, because widl flattens them into the C `Vtbl` struct).

Only the D3D12 surface is considered:
    vkd3d_d3d12.h, vkd3d_d3d12sdklayers.h, vkd3d_d3dcommon.h
The DXGI headers and the vkd3d extension headers belong to other surfaces and
are deliberately excluded.

What is parsed is the C flattened vtable, i.e. the `#else` half of

    #if defined(__cplusplus) && !defined(CINTERFACE)
    MIDL_INTERFACE("....") Name : public Base { ... };
    #else
    typedef struct NameVtbl { ... } NameVtbl;
    #endif

The C++ half is used only to read the direct base name (and to cross-check the
IID).  Note that the C++ half wraps aggregate-return methods in
`#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS` blocks; the C `Vtbl` struct is *not*
guarded and unconditionally carries the explicit return-slot form:

    D3D12_HEAP_DESC * (STDMETHODCALLTYPE *GetDesc)(ID3D12Heap *This,
                                                   D3D12_HEAP_DESC *__ret);

That `__ret` is a real ABI parameter, so it is kept as an ordinary parameter and
the slot is additionally flagged with "aggregate_return": true.

Stdlib only.  Deterministic output.  Self-checks abort with a nonzero exit.
"""

import json
import os
import re
import sys

HEADERS = (
    "vkd3d_d3d12.h",
    "vkd3d_d3d12sdklayers.h",
    "vkd3d_d3dcommon.h",
)

THUNK_DIR = os.path.dirname(os.path.abspath(__file__))
GEN_DIR = os.path.join(os.path.dirname(THUNK_DIR), "idl", "gen")
OUT_PATH = os.path.join(THUNK_DIR, "interfaces.json")

# widl marks the end of a per-interface block with this exact comment.
RE_SECTION_BEGIN = re.compile(r"#define __(\w+)_INTERFACE_DEFINED__$")
RE_SECTION_END = re.compile(r"#endif\s+/\* __(\w+)_INTERFACE_DEFINED__ \*/")

RE_DEFINE_GUID = re.compile(r"DEFINE_GUID\(\s*IID_(\w+)\s*,(.*?)\)\s*;", re.S)
RE_MIDL = re.compile(r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)')
# `Name : public Base` or a bare `Name` (IUnknown).
RE_CPP_DECL = re.compile(r"^(\w+)(?:\s*:\s*public\s+(\w+))?\s*$")

RE_VTBL_BEGIN = re.compile(r"^typedef struct (\w+)Vtbl \{$")
RE_VTBL_END = re.compile(r"^\} (\w+)Vtbl;$")
RE_OWNER_COMMENT = re.compile(r"^/\*\*\* (\w+) methods \*\*\*/$")
# `RET (STDMETHODCALLTYPE *Name)(params)` -- WINAPI is accepted as well since
# widl uses it for some calling conventions.
RE_METHOD = re.compile(
    r"^(?P<ret>.*?)\(\s*(?:STDMETHODCALLTYPE|WINAPI)\s*\*(?P<name>\w+)\s*\)\s*\((?P<params>.*)\)$"
)

RE_COBJ_DEFINE = re.compile(r"^#define\s+(\w+)\((.*?)\)\s*(.*)$")
AGG_MARKER_SUFFIX = "_define_WIDL_C_INLINE_WRAPPERS_for_aggregate_return_support"

# Splits `const FOO **bar[4]` into ("const FOO **", "bar", "[4]").
RE_DECLARATOR = re.compile(r"^(.*?)([A-Za-z_]\w*)((?:\s*\[[^\]]*\])*)$", re.S)


class CheckError(Exception):
    """A self-check failed; the census is not trustworthy."""


def ws(text):
    """Collapse all whitespace runs to a single space and strip the ends."""
    return re.sub(r"\s+", " ", text).strip()


def param_type(param):
    """Strip the parameter name, keeping the type verbatim.

    Used only by the IUnknown signature check, which must not care about widl's
    choice of parameter names (it emits `void **object`, not `ppvObject`).
    """
    m = RE_DECLARATOR.match(param.strip())
    if not m:
        return ws(param)
    return ws(m.group(1) + m.group(3))


def param_name(param):
    """Return the declared parameter name, or '' if it has none."""
    m = RE_DECLARATOR.match(param.strip())
    return m.group(2) if m else ""


def split_params(text):
    """Split a parameter list on top-level commas.

    Depth-aware so that function-pointer-typed parameters (which carry their own
    parenthesised argument lists) survive intact as a single verbatim string.
    """
    out, depth, buf = [], 0, []
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(buf))
            buf = []
        else:
            buf.append(ch)
    tail = "".join(buf)
    if tail.strip() or out:
        out.append(tail)
    return [ws(p) for p in out if ws(p)]


def guid_from_define(args):
    """Assemble canonical 8-4-4-4-12 from the 11 DEFINE_GUID arguments."""
    parts = [a.strip() for a in args.split(",")]
    parts = [p for p in parts if p]
    if len(parts) != 11:
        raise CheckError("DEFINE_GUID with %d args, expected 11: %r" % (len(parts), args))
    vals = [int(p, 16) for p in parts]
    return "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x" % tuple(vals)


def split_sections(lines):
    """Yield (name, start, end) for each `__NAME_INTERFACE_DEFINED__` block."""
    ends = {}
    for i, line in enumerate(lines):
        m = RE_SECTION_END.match(line)
        if m and m.group(1) not in ends:
            ends[m.group(1)] = i
    for i, line in enumerate(lines):
        m = RE_SECTION_BEGIN.match(line)
        if not m:
            continue
        name = m.group(1)
        if name not in ends:
            raise CheckError("interface section %s has no closing #endif" % name)
        yield name, i, ends[name]


def parse_vtbl(name, body):
    """Parse the flattened C vtable struct into a list of slot dicts."""
    slots = []
    owner = None
    inside = False
    pending = []
    for raw in body:
        line = raw.strip()
        if not inside:
            m = RE_VTBL_BEGIN.match(line)
            if m and m.group(1) == name:
                inside = True
            continue
        m = RE_VTBL_END.match(line)
        if m:
            if m.group(1) != name:
                raise CheckError("%s: vtbl closes as %sVtbl" % (name, m.group(1)))
            if pending:
                raise CheckError("%s: unterminated method declaration" % name)
            return slots
        if not line or line in ("BEGIN_INTERFACE", "END_INTERFACE"):
            continue
        m = RE_OWNER_COMMENT.match(line)
        if m and not pending:
            owner = m.group(1)
            continue
        # Declarations may wrap over several lines; join until the `);`.
        pending.append(line)
        if not line.endswith(");"):
            continue
        decl = ws(" ".join(pending))[:-1]  # drop trailing ';'
        pending = []
        m = RE_METHOD.match(decl)
        if not m:
            raise CheckError("%s: cannot parse vtbl entry %r" % (name, decl))
        if owner is None:
            raise CheckError("%s: method %s before any owner comment" % (name, m.group("name")))
        params = split_params(m.group("params"))
        if not params:
            raise CheckError("%s: method %s has no This parameter" % (name, m.group("name")))
        this = params.pop(0)
        if this != "%s *This" % name:
            raise CheckError("%s: first parameter is %r, expected '%s *This'" % (name, this, name))
        slot = {
            "slot": len(slots),
            "owner": owner,
            "name": m.group("name"),
            "ret": ws(m.group("ret")),
            "params": params,
        }
        # An explicit return slot is always the first real parameter.
        if params and param_name(params[0]) == "__ret":
            slot["aggregate_return"] = True
        elif any(param_name(p) == "__ret" for p in params):
            raise CheckError("%s: %s has __ret in a non-leading position" % (name, slot["name"]))
        slots.append(slot)
    raise CheckError("%s: no %sVtbl struct found" % (name, name))


def parse_cobjmacros(name, body):
    """Parse widl's COBJMACROS `#define Iface_Method(This,...)` list.

    Returns {method: (argument count excluding This, is_aggregate_marker)}.
    Only the plain-macro half is read (the `#ifndef WIDL_C_INLINE_WRAPPERS`
    branch); the static-inline half restates the same information.
    """
    macros = {}
    state = 0  # 0 = outside, 1 = in COBJMACROS, 2 = in the plain-macro branch
    for raw in body:
        line = raw.strip()
        if state == 0:
            if line == "#ifdef COBJMACROS":
                state = 1
            continue
        if state == 1:
            if line == "#ifndef WIDL_C_INLINE_WRAPPERS":
                state = 2
            continue
        if line in ("#else", "#endif"):
            break
        m = RE_COBJ_DEFINE.match(line)
        if not m:
            continue
        macro, args, body_text = m.group(1), m.group(2), m.group(3)
        if not macro.startswith(name + "_"):
            continue
        method = macro[len(name) + 1:]
        argv = [a.strip() for a in args.split(",") if a.strip()]
        if not argv or argv[0] != "This":
            raise CheckError("%s: macro %s does not take This" % (name, macro))
        aggregate = body_text.strip() == macro + AGG_MARKER_SUFFIX
        if method in macros:
            raise CheckError("%s: duplicate COBJMACRO for %s" % (name, method))
        macros[method] = (len(argv) - 1, aggregate)
    if state != 2:
        raise CheckError("%s: no COBJMACROS macro list found" % name)
    return macros


def parse_header(fname):
    """Parse one widl header into {interface name: record}."""
    path = os.path.join(GEN_DIR, fname)
    with open(path, "r", encoding="utf-8", errors="strict") as fh:
        lines = fh.read().split("\n")

    result = {}
    for name, start, end in split_sections(lines):
        body = lines[start:end]
        text = "\n".join(body)

        # IID, assembled from the 11 DEFINE_GUID arguments.
        uuid = None
        for m in RE_DEFINE_GUID.finditer(text):
            if m.group(1) == name:
                uuid = guid_from_define(m.group(2))
                break

        # Direct base, from the C++ `Name : public Base` declaration, and the
        # IID restated as a string literal -- a free cross-check.
        base = None
        cpp_uuid = None
        seen_cpp = False
        for i, raw in enumerate(body):
            m = RE_MIDL.match(raw.strip())
            if not m:
                continue
            decl = RE_CPP_DECL.match(body[i + 1].strip())
            if not decl or decl.group(1) != name:
                continue
            cpp_uuid = m.group(1).lower()
            base = decl.group(2)
            seen_cpp = True
            break
        if not seen_cpp:
            raise CheckError("%s: no MIDL_INTERFACE declaration" % name)
        if uuid is None:
            raise CheckError("%s: no DEFINE_GUID(IID_%s, ...)" % (name, name))
        if cpp_uuid != uuid:
            raise CheckError("%s: MIDL_INTERFACE uuid %s != DEFINE_GUID %s" % (name, cpp_uuid, uuid))

        slots = parse_vtbl(name, body)
        macros = parse_cobjmacros(name, body)

        # Secondary base derivation: the owner of the last vtbl section comment
        # that is not the interface itself.  Cross-checked globally, see
        # check_base_sections() -- it can only name an ancestor, not necessarily
        # the direct base.
        prior = [s["owner"] for s in slots if s["owner"] != name]
        base_from_sections = prior[-1] if prior else None

        result[name] = {
            "uuid": uuid,
            "base": base,
            "header": fname,
            "slots": slots,
            "_macros": macros,
            "_base_sections": base_from_sections,
        }
    return result


# --- self-checks -------------------------------------------------------------

def check_iunknown_prefix(census, problems):
    """1. Slots 0/1/2 are always IUnknown's QueryInterface/AddRef/Release.

    Signatures are compared by type, not by parameter name: widl spells the
    QueryInterface out-parameter `void **object` in these headers.
    """
    want = (
        ("QueryInterface", "HRESULT", ["REFIID", "void **"]),
        ("AddRef", "ULONG", []),
        ("Release", "ULONG", []),
    )
    for name, iface in sorted(census.items()):
        slots = iface["slots"]
        if len(slots) < 3:
            problems.append("%s: only %d slots, cannot be a COM interface" % (name, len(slots)))
            continue
        for idx, (mname, mret, mparams) in enumerate(want):
            s = slots[idx]
            got = [param_type(p) for p in s["params"]]
            if (s["owner"], s["name"], s["ret"], got) != ("IUnknown", mname, mret, mparams):
                problems.append(
                    "%s: slot %d is %s.%s %s(%s), expected IUnknown.%s %s(%s)"
                    % (name, idx, s["owner"], s["name"], s["ret"], ", ".join(got),
                       mname, mret, ", ".join(mparams))
                )


def check_cobjmacros(census, problems):
    """2. The COBJMACRO list must agree with the vtbl, one method to one macro.

    This is an independent read of the same interface, so it catches both
    dropped slots and mis-split parameter lists.  It also gives a second,
    `__ret`-free detector for aggregate-return methods: widl replaces those
    macros with a `..._for_aggregate_return_support` marker token.
    """
    for name, iface in sorted(census.items()):
        macros = iface["_macros"]
        by_name = {}
        for s in iface["slots"]:
            if s["name"] in by_name:
                problems.append("%s: duplicate method name %s in vtbl" % (name, s["name"]))
            by_name[s["name"]] = s
        if set(by_name) != set(macros):
            only_vtbl = sorted(set(by_name) - set(macros))
            only_macro = sorted(set(macros) - set(by_name))
            problems.append(
                "%s: method sets differ; vtbl-only=%s macro-only=%s"
                % (name, only_vtbl, only_macro)
            )
            continue
        for mname, s in sorted(by_name.items()):
            argc, macro_agg = macros[mname]
            vtbl_agg = bool(s.get("aggregate_return"))
            if macro_agg != vtbl_agg:
                problems.append(
                    "%s.%s: aggregate_return mismatch (__ret=%s, macro marker=%s)"
                    % (name, mname, vtbl_agg, macro_agg)
                )
            # The macro form never exposes __ret, so drop it before comparing.
            want = len(s["params"]) - (1 if vtbl_agg else 0)
            if argc != want:
                problems.append(
                    "%s.%s: macro takes %d args, vtbl has %d"
                    % (name, mname, argc, want)
                )


def check_chains(census, problems, notes):
    """3. Inheritance chains grow, and a derived vtbl starts with its base's.

    The prefix comparison is on (owner, name, arity) per slot, which is what a
    thunk generator actually depends on: a base pointer must be usable through a
    derived vtable for the first N slots.

    Note on "strictly increasing": ID3D12Pageable derives from ID3D12DeviceChild
    and genuinely adds no methods (it is a pure marker interface in D3D12), so a
    strict > would reject valid input.  The check is therefore >= -- a chain may
    never shrink -- and every zero-method derivation is reported explicitly in
    the summary so it can never pass unnoticed.
    """
    for name, iface in sorted(census.items()):
        base = iface["base"]
        if base is None:
            continue
        if base not in census:
            continue  # reported by check_graph
        bslots = census[base]["slots"]
        dslots = iface["slots"]
        if len(dslots) < len(bslots):
            problems.append(
                "%s has %d slots but base %s has %d"
                % (name, len(dslots), base, len(bslots))
            )
            continue
        if len(dslots) == len(bslots):
            notes.append("%s adds no methods over %s" % (name, base))
        for i, bs in enumerate(bslots):
            ds = dslots[i]
            bkey = (bs["owner"], bs["name"], len(bs["params"]))
            dkey = (ds["owner"], ds["name"], len(ds["params"]))
            if bkey != dkey:
                problems.append(
                    "%s slot %d is %s.%s/%d but base %s has %s.%s/%d"
                    % (name, i, dkey[0], dkey[1], dkey[2], base, bkey[0], bkey[1], bkey[2])
                )
                break


def check_graph(census, problems):
    """4. Unique names, every IID resolved, every base present (or IUnknown)."""
    for name, iface in sorted(census.items()):
        if not iface["uuid"]:
            problems.append("%s: missing uuid" % name)
        base = iface["base"]
        if base is None:
            if name != "IUnknown":
                problems.append("%s: no base interface but is not IUnknown" % name)
            continue
        if base not in census and base != "IUnknown":
            problems.append("%s: base %s is not in the census" % (name, base))
    # Every declared owner must resolve too -- it names a real interface.
    known = set(census)
    for name, iface in sorted(census.items()):
        for s in iface["slots"]:
            if s["owner"] not in known:
                problems.append("%s: slot %d owned by unknown interface %s"
                                % (name, s["slot"], s["owner"]))
                break


def check_base_sections(census, problems):
    """4b. The C++ base must be corroborated by the vtbl's section comments.

    The owner of the last `/*** X methods ***/` comment before an interface's
    own section is normally its direct base -- but widl only emits a section for
    an interface that actually declares methods.  ID3D12Heap : ID3D12Pageable
    shows `/*** ID3D12DeviceChild methods ***/` last, because ID3D12Pageable
    contributes nothing.  So the requirement is that the section-derived name is
    the declared base or one of its ancestors, with every interface skipped over
    on the way contributing zero methods.
    """
    for name, iface in sorted(census.items()):
        seen = iface["_base_sections"]
        base = iface["base"]
        if base is None:
            if seen is not None:
                problems.append("%s: baseless but vtbl shows section owner %s" % (name, seen))
            continue
        cur = base
        guard = 0
        while cur is not None and cur != seen:
            if cur not in census:
                cur = None
                break
            parent = census[cur]["base"]
            # cur was skipped in the section comments, so it must add nothing.
            if parent is not None and parent in census:
                if len(census[cur]["slots"]) != len(census[parent]["slots"]):
                    break
            cur = parent
            guard += 1
            if guard > 64:
                break
        if cur != seen:
            problems.append(
                "%s: declared base %s not corroborated by vtbl sections (last section owner %r)"
                % (name, base, seen)
            )


def main():
    census = {}
    dupes = []
    for fname in HEADERS:
        for name, rec in parse_header(fname).items():
            if name in census:
                dupes.append("%s defined in both %s and %s" % (name, census[name]["header"], fname))
            census[name] = rec

    problems = list(dupes)
    notes = []
    check_iunknown_prefix(census, problems)
    check_cobjmacros(census, problems)
    check_chains(census, problems, notes)
    check_graph(census, problems)
    check_base_sections(census, problems)

    if problems:
        sys.stderr.write("SELF-CHECK FAILED (%d problem(s)):\n" % len(problems))
        for p in problems:
            sys.stderr.write("  - %s\n" % p)
        return 1

    # --- 5. summary ----------------------------------------------------------
    total_slots = sum(len(i["slots"]) for i in census.values())
    aggregates = sorted(
        "%s.%s" % (n, s["name"])
        for n, i in census.items()
        for s in i["slots"]
        if s.get("aggregate_return")
    )
    largest = max(sorted(census), key=lambda n: len(census[n]["slots"]))
    per_header = {}
    for i in census.values():
        per_header[i["header"]] = per_header.get(i["header"], 0) + 1

    print("interfaces:          %d" % len(census))
    print("vtable slots:        %d" % total_slots)
    print("aggregate returns:   %d" % len(aggregates))
    for a in aggregates:
        print("    %s" % a)
    print("largest interface:   %s (%d slots)" % (largest, len(census[largest]["slots"])))
    print("per header:")
    for h in HEADERS:
        print("    %-24s %d" % (h, per_header.get(h, 0)))
    if notes:
        print("notes:")
        for n in notes:
            print("    %s" % n)

    out = {"interfaces": {}}
    for name in sorted(census):
        rec = {k: v for k, v in census[name].items() if not k.startswith("_")}
        out["interfaces"][name] = rec
    with open(OUT_PATH, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=2, ensure_ascii=True, sort_keys=False)
        fh.write("\n")
    print("wrote %s" % OUT_PATH)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except CheckError as exc:
        sys.stderr.write("SELF-CHECK FAILED: %s\n" % exc)
        sys.exit(1)
