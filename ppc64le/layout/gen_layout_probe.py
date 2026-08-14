#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
gen_layout_probe.py -- emit a C11 struct-layout probe for the D3D12 API surface.

Why this exists
---------------
An emulated x86-64 game hands D3D12 structs to a *native* ppc64le vkd3d-proton.
The emulator can translate registers and instructions, but it cannot fix up a
struct whose members sit at different byte offsets on the two sides.  This tool
enumerates every aggregate and enum that crosses the D3D12 API boundary and
emits a plain C program (probe.c) that prints its size, alignment and every
member offset.  Compile and run the *same* probe.c on x86-64 (SysV) and on
ppc64le (ELFv2); if the two outputs are byte-identical, the ABI boundary is
layout-safe.  check.sh drives that comparison.

Design notes
------------
* Input is the widl-generated headers in ppc64le/idl/gen plus include/vkd3d_windows.h
  (which defines GUID, used by value in several D3D12 structs).
* We parse the headers directly (stdlib only, no compiler needed) but we do
  honour #if/#ifdef/#else/#endif, because the headers hide the COM C++ view
  behind `#if defined(__cplusplus) && !defined(CINTERFACE)` and hide LUID/RECT
  behind `#if !defined(_WIN32)`.  Every macro is treated as undefined unless the
  header itself #defines it in an active region -- which is exactly the state of
  the world when the probe is compiled as C on Linux.
* COM interfaces and their Vtbl structs are excluded: they are vtable plumbing,
  not data crossing the boundary.  (A vtable *does* have an ABI, but its layout
  is fixed by the header, and calls through it are handled by the thunking layer,
  not by struct copying.)
* Anonymous unions/structs are recursed into: C11 lets us say
  offsetof(D3D12_RESOURCE_BARRIER, Transition) even though `Transition` lives in
  an unnamed union.  Named-but-anonymously-typed members (D3D12_INDIRECT_ARGUMENT_DESC
  has several) get both their own MEMBER line and a recursion into their leaves.
* Bitfields cannot be fed to offsetof.  Instead of giving up, the probe fills the
  record with 0xff, writes 0 to the bitfield and reports which bits went to zero.
  That yields the *real* bit position and width at runtime, so a bitfield
  allocation difference is caught rather than merely noted.

Output (all beside this script):
    probe.c      the generated C11 probe
    records.txt  human-readable coverage listing
"""

import os
import re
import sys

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))

GEN = os.path.join(REPO, "ppc64le", "idl", "gen")
INC = os.path.join(REPO, "include")

# Parsed for records/enums/typedefs.  Order matters only for dedup diagnostics.
HEADERS = [
    os.path.join(INC, "vkd3d_windows.h"),
    os.path.join(GEN, "vkd3d_dxgiformat.h"),
    os.path.join(GEN, "vkd3d_dxgibase.h"),
    os.path.join(GEN, "vkd3d_d3dcommon.h"),
    os.path.join(GEN, "vkd3d_d3d12.h"),
    os.path.join(GEN, "vkd3d_d3d12sdklayers.h"),
]

# Headers the generated probe.c #includes.  vkd3d_windows.h must come first: the
# widl headers do `#ifndef COM_NO_WINDOWS_H / #include <windows.h>` before they
# include vkd3d_windows.h themselves, and it is vkd3d_windows.h that defines
# COM_NO_WINDOWS_H on non-Windows.
PROBE_INCLUDES = [
    "vkd3d_windows.h",
    "vkd3d_dxgiformat.h",
    "vkd3d_dxgibase.h",
    "vkd3d_d3dcommon.h",
    "vkd3d_d3d12.h",
    "vkd3d_d3d12sdklayers.h",
]

# Records that must be present or the run is meaningless.
REQUIRED = [
    "D3D12_RESOURCE_DESC",
    "D3D12_ROOT_SIGNATURE_DESC",
    "D3D12_GRAPHICS_PIPELINE_STATE_DESC",
    "D3D12_CLEAR_VALUE",
    "D3D12_RESOURCE_BARRIER",
    "D3D12_CPU_DESCRIPTOR_HANDLE",
    "D3D12_GPU_DESCRIPTOR_HANDLE",
]

# widl spells nameless aggregates with these macros; both expand to nothing.
NAMELESS_QUALIFIER = "__C89_NAMELESS"
NAMELESS_NAMES = ("__C89_NAMELESSUNIONNAME", "__C89_NAMELESSSTRUCTNAME")

# Declaration noise that carries no layout meaning for our purposes.
DECL_NOISE = {
    "const", "volatile", "struct", "union", "enum", "interface",
    "CONST_VTBL", "BEGIN_INTERFACE", "END_INTERFACE",
}

BUILTIN_SCALARS = {
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "_Bool", "size_t", "ptrdiff_t", "wchar_t",
}


class Fail(Exception):
    pass


def fail(msg):
    raise Fail(msg)


# --------------------------------------------------------------------------
# Lexical pre-pass: comments, then preprocessor conditionals
# --------------------------------------------------------------------------

def strip_comments(src):
    """Replace comments with spaces, preserving newlines (and thus line numbers)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            i += 2
            out.append(" ")
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


_DEFINED_RE = re.compile(r"\bdefined\s*(?:\(\s*(\w+)\s*\)|(\w+))")
_IDENT_RE = re.compile(r"\b[A-Za-z_]\w*\b")


def eval_cond(expr, defined):
    """Evaluate a #if expression with 'unknown macro == 0' semantics.

    Only the constructs these headers actually use need to work: defined(),
    !, &&, ||, parentheses and integer literals.  Anything we cannot make sense
    of is a hard error rather than a silent guess -- a mis-evaluated conditional
    would silently drop records from the probe.
    """
    expr = _DEFINED_RE.sub(lambda m: "1" if (m.group(1) or m.group(2)) in defined else "0", expr)
    expr = _IDENT_RE.sub("0", expr)          # any surviving identifier is undefined
    expr = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    expr = re.sub(r"\bnot=", "!=", expr)      # undo damage to '!='
    expr = re.sub(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]+", r"\1", expr)
    if not re.fullmatch(r"[\s0-9xXa-fA-F()!=<>+\-*/|&^~]*(?:and|or|not|[\s0-9xXa-fA-F()!=<>+\-*/|&^~])*", expr):
        fail("cannot evaluate preprocessor condition: %r" % expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))
    except Exception as exc:
        fail("cannot evaluate preprocessor condition %r: %s" % (expr, exc))


def select_active(src, path):
    """Blank out lines inside inactive #if regions and drop all directives.

    Returns (text, defined_macros).  Blanking rather than deleting keeps the
    text offsets stable, which keeps error messages honest.
    """
    defined = set()
    out = []
    # stack of [active_here, any_branch_taken_yet, parent_active]
    stack = []

    def active():
        return all(f[0] for f in stack)

    for line in src.split("\n"):
        stripped = line.strip()
        m = re.match(r"#\s*(\w+)\s*(.*)$", stripped)
        if not m:
            out.append(line if active() else "")
            continue
        directive, rest = m.group(1), m.group(2).strip()

        if directive in ("if", "ifdef", "ifndef"):
            parent = active()
            if not parent:
                val = False
            elif directive == "ifdef":
                val = rest.split()[0] in defined if rest else False
            elif directive == "ifndef":
                val = rest.split()[0] not in defined if rest else True
            else:
                val = eval_cond(rest, defined)
            stack.append([val, val, parent])
        elif directive == "elif":
            if not stack:
                fail("%s: #elif without #if" % path)
            frame = stack[-1]
            if not frame[2] or frame[1]:
                frame[0] = False
            else:
                frame[0] = eval_cond(rest, defined)
                frame[1] = frame[1] or frame[0]
        elif directive == "else":
            if not stack:
                fail("%s: #else without #if" % path)
            frame = stack[-1]
            frame[0] = frame[2] and not frame[1]
            frame[1] = frame[1] or frame[0]
        elif directive == "endif":
            if not stack:
                fail("%s: #endif without #if" % path)
            stack.pop()
        elif directive == "define" and active():
            name = re.match(r"(\w+)", rest)
            if name:
                defined.add(name.group(1))
        elif directive == "undef" and active():
            name = re.match(r"(\w+)", rest)
            if name:
                defined.discard(name.group(1))
        out.append("")
    if stack:
        fail("%s: unterminated #if (depth %d)" % (path, len(stack)))
    return "\n".join(out), defined


# --------------------------------------------------------------------------
# Declaration model
# --------------------------------------------------------------------------

class Member(object):
    """One declarator inside a record body."""

    __slots__ = ("kind", "name", "type_text", "base_type", "is_pointer",
                 "array", "bit_width", "children", "raw")

    def __init__(self, kind, name=None, type_text="", base_type=None,
                 is_pointer=False, array="", bit_width=None, children=None, raw=""):
        self.kind = kind                  # 'field' | 'aggregate'
        self.name = name                  # None for anonymous aggregates
        self.type_text = type_text
        self.base_type = base_type        # identifier used for the closure check
        self.is_pointer = is_pointer
        self.array = array                # e.g. '[3][4]'
        self.bit_width = bit_width        # int for bitfields, else None
        self.children = children or []
        self.raw = raw


class Record(object):
    __slots__ = ("name", "tag", "kind", "members", "header")

    def __init__(self, name, tag, kind, members, header):
        self.name = name
        self.tag = tag
        self.kind = kind                  # 'struct' | 'union'
        self.members = members
        self.header = header


def match_brace(text, start):
    """Index of the '}' matching the '{' at `start`."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    fail("unbalanced braces starting at offset %d" % start)


def split_declarations(body):
    """Split a record body into declarations on top-level semicolons."""
    decls = []
    depth = 0
    cur = []
    for ch in body:
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        if ch == ";" and depth == 0:
            decls.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        fail("trailing text in record body: %r" % tail)
    return [d.strip() for d in decls if d.strip()]


def clean_name(tok):
    tok = tok.strip()
    if tok in NAMELESS_NAMES or tok == "":
        return None
    return tok


def parse_declarator(text):
    """Split 'const FOO *bar[4]' into (type_text, name, array, is_pointer)."""
    text = " ".join(text.split())
    m = re.match(r"^(.*?)((?:\s*\[[^\]]*\])+)\s*$", text)
    array = ""
    if m:
        array = "".join(m.group(2).split())
        text = m.group(1).strip()
    if "(" in text:
        # function-pointer declarator: TYPE (CALLCONV *name)(args)
        fp = re.search(r"\(\s*[\w\s]*\*\s*(\w+)\s*\)", text)
        if not fp:
            fail("unparsable declarator: %r" % text)
        return text, fp.group(1), array, True
    toks = text.split()
    if not toks:
        fail("empty declarator")
    name = toks[-1].lstrip("*")
    if not re.fullmatch(r"[A-Za-z_]\w*", name):
        fail("unparsable declarator name in %r" % text)
    is_pointer = "*" in text
    type_text = " ".join(toks[:-1]) if len(toks) > 1 else ""
    if toks[-1].startswith("*"):
        type_text = (type_text + " " + "*" * (len(toks[-1]) - len(name))).strip()
    return type_text, name, array, is_pointer


def base_type_of(type_text):
    """Identifier a value member's type ultimately names, or None for builtins."""
    toks = [t for t in type_text.replace("*", " ").split() if t not in DECL_NOISE]
    idents = [t for t in toks if re.fullmatch(r"[A-Za-z_]\w*", t)]
    if not idents:
        return None
    named = [t for t in idents if t not in BUILTIN_SCALARS]
    if not named:
        return None
    return named[-1]


def parse_members(body, where):
    members = []
    for decl in split_declarations(body):
        d = decl.strip()
        if d.startswith(NAMELESS_QUALIFIER):
            d = d[len(NAMELESS_QUALIFIER):].strip()
        agg = re.match(r"^(struct|union)\b([^{;]*)\{", d)
        if agg and "{" in d:
            open_i = d.index("{")
            close_i = match_brace(d, open_i)
            inner = d[open_i + 1:close_i]
            declarator = d[close_i + 1:].strip()
            if "[" in declarator:
                fail("%s: array of anonymous aggregate is not handled: %r" % (where, decl))
            name = clean_name(declarator)
            if declarator and name and not re.fullmatch(r"[A-Za-z_]\w*", name):
                fail("%s: unexpected aggregate declarator %r" % (where, declarator))
            members.append(Member(
                kind="aggregate",
                name=name,
                type_text=agg.group(1),
                children=parse_members(inner, where),
                raw=decl,
            ))
            continue
        if "{" in d:
            fail("%s: unexpected brace in declaration %r" % (where, decl))
        bit_width = None
        if ":" in d:
            head, _, width = d.rpartition(":")
            width = width.strip()
            if not re.fullmatch(r"\d+", width):
                fail("%s: unparsable bitfield width in %r" % (where, decl))
            bit_width = int(width)
            d = head.strip()
        type_text, name, array, is_pointer = parse_declarator(d)
        members.append(Member(
            kind="field",
            name=name,
            type_text=type_text,
            base_type=None if is_pointer else base_type_of(type_text),
            is_pointer=is_pointer,
            array=array,
            bit_width=bit_width,
            raw=decl,
        ))
    return members


# --------------------------------------------------------------------------
# Header scan
# --------------------------------------------------------------------------

RECORD_RE = re.compile(r"^[ \t]*typedef[ \t\n]+(struct|union)\b[ \t\n]*(\w+)?[ \t\n]*\{", re.M)
ENUM_RE = re.compile(r"^[ \t]*typedef[ \t\n]+enum\b[ \t\n]*(\w+)?[ \t\n]*\{", re.M)
SIMPLE_TYPEDEF_RE = re.compile(r"^[ \t]*typedef[ \t]+([^{};]*);", re.M)


def trailing_name(text, close_i, path):
    """Read 'NAME;' following a record/enum closing brace."""
    rest = text[close_i + 1:]
    m = re.match(r"\s*([^;]*);", rest)
    if not m:
        fail("%s: no typedef name after closing brace at offset %d" % (path, close_i))
    decl = " ".join(m.group(1).split())
    if "," in decl:
        fail("%s: multiple typedef names not handled: %r" % (path, decl))
    if not re.fullmatch(r"[A-Za-z_]\w*", decl):
        fail("%s: unexpected typedef declarator %r" % (path, decl))
    return decl


def is_interface_like(name, tag, body):
    """COM vtable plumbing, not data that crosses the API boundary.

    Vtbl structs are full of function pointers wrapped in BEGIN_INTERFACE /
    END_INTERFACE markers; the interface structs themselves are `interface IFoo
    { CONST_VTBL IFooVtbl *lpVtbl; }` and are not typedefs, so in practice only
    the Vtbl rule fires -- the lpVtbl rule is belt and braces.
    """
    if name.endswith("Vtbl") or (tag or "").endswith("Vtbl"):
        return True
    if re.match(r"^I[A-Z]", name) and re.search(r"\blpVtbl\b", body):
        return True
    return False


def scan_header(path):
    """Return (records, enums, typedefs, excluded) found in one header."""
    raw = open(path, "r", encoding="utf-8", errors="replace").read()
    text, _ = select_active(strip_comments(raw), path)
    base = os.path.basename(path)

    records, enums, typedefs, excluded = [], [], {}, []

    for m in RECORD_RE.finditer(text):
        kind, tag = m.group(1), m.group(2)
        open_i = text.index("{", m.start())
        close_i = match_brace(text, open_i)
        name = trailing_name(text, close_i, path)
        body = text[open_i + 1:close_i]
        if is_interface_like(name, tag, body):
            excluded.append((name, base, "COM interface/vtable"))
            continue
        members = parse_members(body, "%s:%s" % (base, name))
        records.append(Record(name, tag, kind, members, base))

    for m in ENUM_RE.finditer(text):
        open_i = text.index("{", m.start())
        close_i = match_brace(text, open_i)
        enums.append((trailing_name(text, close_i, path), m.group(1), base))

    for m in SIMPLE_TYPEDEF_RE.finditer(text):
        decl = " ".join(m.group(1).split())
        if not decl:
            continue
        fp = re.search(r"\(\s*[\w\s]*\*\s*(\w+)\s*\)", decl)
        if fp:
            typedefs.setdefault(fp.group(1), ("pointer", None))
            continue
        decl_no_array = re.sub(r"\[[^\]]*\]", "", decl)
        toks = decl_no_array.split()
        if len(toks) < 2:
            continue
        alias = toks[-1].lstrip("*")
        if not re.fullmatch(r"[A-Za-z_]\w*", alias):
            continue
        if "*" in decl_no_array:
            typedefs.setdefault(alias, ("pointer", None))
        else:
            typedefs.setdefault(alias, ("alias", base_type_of(" ".join(toks[:-1]))))

    return records, enums, typedefs, excluded


# --------------------------------------------------------------------------
# Probe emission
# --------------------------------------------------------------------------

class Probe(object):
    """Flattened list of things to print, in emission order."""

    def __init__(self):
        self.lines = []        # (kind, payload) tuples consumed by the C writer
        self.n_members = 0
        self.n_bitfields = 0


def walk_members(rec, members, prefix, probe, seen):
    for m in members:
        if m.kind == "aggregate":
            if m.name:
                path = prefix + m.name
                probe.lines.append(("member", rec.name, path))
                probe.n_members += 1
                walk_members(rec, m.children, path + ".", probe, seen)
            else:
                walk_members(rec, m.children, prefix, probe, seen)
            continue
        path = prefix + m.name
        if path in seen:
            fail("%s: duplicate member path %s" % (rec.name, path))
        seen.add(path)
        if m.bit_width is not None:
            probe.lines.append(("bitfield", rec.name, path, m.bit_width))
            probe.n_bitfields += 1
        else:
            probe.lines.append(("member", rec.name, path))
            probe.n_members += 1


C_PROLOGUE = """\
/* Generated by ppc64le/layout/gen_layout_probe.py -- do not edit.
 *
 * Prints the size, alignment and member offsets of every struct, union and enum
 * on the D3D12 API boundary.  Build and run the SAME file on x86-64 and on
 * ppc64le; identical output means an emulated x86-64 game and a native ppc64le
 * vkd3d-proton agree on every byte of every descriptor they exchange.
 *
 *   gcc -std=c11 -I ppc64le/idl/gen -I include probe.c -o probe && ./probe
 */

#define COM_NO_WINDOWS_H
%(includes)s
#include <stdio.h>
#include <stddef.h>
#include <stdalign.h>
#include <string.h>

static void emit_record(const char *name, size_t size, size_t align)
{
    printf("RECORD %%s size=%%zu align=%%zu\\n", name, size, align);
}

static void emit_member(const char *rec, const char *mem, size_t off, size_t size)
{
    printf("MEMBER %%s.%%s off=%%zu size=%%zu\\n", rec, mem, off, size);
}

static void emit_enum(const char *name, size_t size)
{
    printf("ENUM %%s size=%%zu\\n", name, size);
}

/* offsetof() is illegal on a bitfield, so measure it the hard way: fill the
 * record with 1 bits, store 0 into the field, and report which bits dropped.
 * That gives the real bit position and width the compiler chose, which is what
 * an ABI comparison actually needs. */
static void emit_bitfield(const char *rec, const char *mem, unsigned declared,
        const unsigned char *bytes, size_t n)
{
    size_t i;
    int bit, first = -1, last = -1, count = 0;

    for (i = 0; i < n; ++i)
    {
        for (bit = 0; bit < 8; ++bit)
        {
            if (!(bytes[i] & (1u << bit)))
            {
                int index = (int)(i * 8u) + bit;
                if (first < 0)
                    first = index;
                last = index;
                ++count;
            }
        }
    }
    printf("BITFIELD %%s.%%s declared=%%u firstbit=%%d lastbit=%%d bits=%%d\\n",
            rec, mem, declared, first, last, count);
}

"""


def c_ident(name):
    return re.sub(r"\W", "_", name)


def emit_c(records, enums, probe, path):
    out = []
    out.append(C_PROLOGUE % {
        "includes": "\n".join("#include <%s>" % h for h in PROBE_INCLUDES),
    })

    # One function per record keeps main() from becoming a single enormous body.
    by_record = {}
    for entry in probe.lines:
        by_record.setdefault(entry[1], []).append(entry)

    for rec in records:
        out.append("static void probe_%s(void)\n{\n" % c_ident(rec.name))
        out.append('    emit_record("%s", sizeof(%s), alignof(%s));\n'
                   % (rec.name, rec.name, rec.name))
        for entry in by_record.get(rec.name, []):
            if entry[0] == "member":
                path_ = entry[2]
                out.append('    emit_member("%s", "%s", offsetof(%s, %s), sizeof(((%s *)0)->%s));\n'
                           % (rec.name, path_, rec.name, path_, rec.name, path_))
            else:
                path_, width = entry[2], entry[3]
                out.append("    {\n")
                out.append("        union { %s v; unsigned char b[sizeof(%s)]; } u;\n"
                           % (rec.name, rec.name))
                out.append("        memset(&u, 0xff, sizeof(u));\n")
                out.append("        u.v.%s = 0;\n" % path_)
                out.append('        emit_bitfield("%s", "%s", %uu, u.b, sizeof(u.b));\n'
                           % (rec.name, path_, width))
                out.append("    }\n")
        out.append("}\n\n")

    out.append("int main(void)\n{\n")
    for rec in records:
        out.append("    probe_%s();\n" % c_ident(rec.name))
    if records and enums:
        out.append("\n")
    for name, _tag, _hdr in enums:
        out.append('    emit_enum("%s", sizeof(%s));\n' % (name, name))
    out.append('\n    printf("TOTAL records=%d members=%d enums=%d bitfields=%d\\n");\n'
               % (len(records), probe.n_members, len(enums), probe.n_bitfields))
    out.append("    return 0;\n}\n")

    with open(path, "w", encoding="utf-8") as fh:
        fh.write("".join(out))


def emit_records_txt(records, enums, probe, path):
    counts = {}
    for entry in probe.lines:
        counts[entry[1]] = counts.get(entry[1], 0) + 1
    # A handful of raytracing names run to ~80 characters; padding every row out
    # to that would make the file unreadable, so cap and let outliers overflow.
    width = min(60, max([len(r.name) for r in records] + [len(e[0]) for e in enums] + [1]))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("D3D12 API-boundary layout coverage\n")
        fh.write("==================================\n\n")
        fh.write("structs/unions probed : %d\n" % len(records))
        fh.write("enums probed          : %d\n" % len(enums))
        fh.write("member probes         : %d\n" % probe.n_members)
        fh.write("bitfield probes       : %d\n" % probe.n_bitfields)
        fh.write("\n--- records (member probes per record) ---\n\n")
        for rec in records:
            fh.write("%-*s  members=%-4d %-6s %s\n"
                     % (width, rec.name, counts.get(rec.name, 0), rec.kind, rec.header))
        fh.write("\n--- enums ---\n\n")
        for name, _tag, hdr in enums:
            fh.write("%-*s  %s\n" % (width, name, hdr))


# --------------------------------------------------------------------------
# Self-checks
# --------------------------------------------------------------------------

def check_closure(records, record_names, enum_names, typedefs, incomplete):
    """Every aggregate reachable by value from a probed record must be probed."""
    problems = []

    def resolve(name, depth=0):
        if depth > 16:
            return name
        if name in record_names or name in enum_names:
            return name
        entry = typedefs.get(name)
        if entry is None:
            return name
        kind, target = entry
        if kind == "pointer" or target is None:
            return None
        return resolve(target, depth + 1)

    def visit(rec, members, prefix):
        for m in members:
            if m.kind == "aggregate":
                visit(rec, m.children, prefix + ((m.name + ".") if m.name else ""))
                continue
            if m.is_pointer or m.base_type is None:
                continue
            target = resolve(m.base_type)
            if target is None:
                continue
            if target in record_names or target in enum_names:
                continue
            if target in BUILTIN_SCALARS:
                continue
            if target in incomplete:
                problems.append("%s.%s%s: value member of incomplete type %s"
                                % (rec.name, prefix, m.name, m.base_type))
                continue
            if target in typedefs:
                continue
            problems.append("%s.%s%s: value member of unknown type %s (resolved %s)"
                            % (rec.name, prefix, m.name, m.base_type, target))

    for rec in records:
        visit(rec, rec.members, "")
    return problems


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def main():
    all_records = []
    all_enums = []
    typedefs = {}
    seen_records = {}
    seen_enums = {}
    excluded = []

    for path in HEADERS:
        if not os.path.exists(path):
            fail("missing input header: %s" % path)
        records, enums, tds, skipped = scan_header(path)
        excluded.extend(skipped)
        for name, base in tds.items():
            typedefs.setdefault(name, base)
        for rec in records:
            prev = seen_records.get(rec.name)
            if prev is not None:
                # Same typedef seen twice (headers include each other); the
                # bodies must agree or our dedup would be hiding a difference.
                if [m.raw for m in prev.members] != [m.raw for m in rec.members]:
                    fail("conflicting definitions of %s in %s and %s"
                         % (rec.name, prev.header, rec.header))
                continue
            seen_records[rec.name] = rec
            all_records.append(rec)
        for name, tag, hdr in enums:
            if name in seen_enums:
                continue
            seen_enums[name] = hdr
            all_enums.append((name, tag, hdr))

    if not all_records:
        fail("no records parsed -- the header format must have changed")

    all_records.sort(key=lambda r: r.name)
    all_enums.sort(key=lambda e: e[0])

    record_names = set(seen_records)
    for rec in all_records:
        if rec.tag:
            record_names.add(rec.tag)
    enum_names = set(seen_enums)
    for _name, tag, _hdr in all_enums:
        if tag:
            enum_names.add(tag)

    # Typedefs that name a struct/union but never got a body (forward decls,
    # `typedef interface IFoo IFoo;`).  Legal as pointers only.
    incomplete = set()
    for name, (kind, target) in typedefs.items():
        if kind == "alias" and target == name and name not in record_names:
            incomplete.add(name)

    # --- self-check: closure over value-typed members
    problems = check_closure(all_records, record_names, enum_names, typedefs, incomplete)
    if problems:
        fail("closure check failed:\n  " + "\n  ".join(problems))

    # --- self-check: the structs everything else hangs off must be present
    missing = [n for n in REQUIRED if n not in seen_records]
    if missing:
        fail("required records missing from probe: %s" % ", ".join(missing))

    probe = Probe()
    for rec in all_records:
        walk_members(rec, rec.members, "", probe, set())

    probe_c = os.path.join(HERE, "probe.c")
    records_txt = os.path.join(HERE, "records.txt")
    emit_c(all_records, all_enums, probe, probe_c)
    emit_records_txt(all_records, all_enums, probe, records_txt)

    bitfields = [(e[1], e[2], e[3]) for e in probe.lines if e[0] == "bitfield"]

    print("wrote %s" % os.path.relpath(probe_c, REPO))
    print("wrote %s" % os.path.relpath(records_txt, REPO))
    print("records (struct/union): %d" % len(all_records))
    print("members probed        : %d" % probe.n_members)
    print("enums                 : %d" % len(all_enums))
    print("bitfields             : %d" % probe.n_bitfields)
    for rec, mem, width in bitfields:
        print("  bitfield %s.%s : %d" % (rec, mem, width))
    print("excluded (interfaces) : %d" % len(excluded))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as exc:
        sys.stderr.write("gen_layout_probe.py: FAIL: %s\n" % exc)
        sys.exit(2)
