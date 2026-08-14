# POWER8 ISA floor — scan result and the one recorded exception

Scan: dxvk-ppc64le's `scan-isa.sh` (objdump-oracle version, self-test A 5/5,
B suppressed, C 1/1), run unmodified against `build-native/` on op4k,
2026-08-13, after `./ppc64le/build-native.sh` (default `-mcpu=power8`).

Result: **one finding, and it is switch-table data, not code.** [MEASURED]

```
POWER8 FLOOR VIOLATED
  xsiexpqp  1 site(s), first at .../opcodes_opcodes_dxil_builtins.cpp.o (.text+415c)
```

Disassembly of the site at `-Mpower10`:

```
4148: lwax r9,r10,r9        ; switch dispatch: load 32-bit offset
414c: add  r9,r9,r10
4150: mtctr r9
4154: bctr                   ; end of real code
4158: .word 0xffffffb8       ; -72   — decodes as fmsub at the p8 floor
415c: .word 0xfffffec8       ; -312  — decodes as xsiexpqp at p10 only
4160+ .word 0xfffffbe8 ...   ; -1048 × many — decode at no level
```

This is precisely the scanner's documented soft edge ("a run of words that
decode only above the floor is treated as data iff the words on BOTH sides
decode at no ISA level"): the table entry at `+4158` happens to decode as
`fmsub` at the power8 floor, so the wall rule cannot fire for `+415c`, and
the scanner reports it — erring loud, as designed. The two words are GCC
jump-table offsets (-72, -312) for the DXIL-builtin switch, following an
unconditional `bctr`; no execution path reaches them as instructions.

Flag propagation is not in question: `grep -o -- -mcpu= build.ninja` shows
**162 × `-mcpu=power8` and nothing else** — the meson native file's
[built-in options] reach the dxil-spirv subproject too. [MEASURED]

Verdict: the tree is clean at the POWER8 floor. Re-run after toolchain or
dxil-spirv bumps; if this same table moves, the finding may change shape or
disappear — any NEW mnemonic or site must be investigated from scratch, not
waved through by analogy to this one.
