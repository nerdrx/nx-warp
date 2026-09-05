#!/usr/bin/env python3
"""shader-lint.py -- portability checks for the NX Warp GLSL compute shaders.

Everything here exists because of one device: the Adreno 650 in the Pico 4.
The rules and the evidence behind them are in docs/ADRENO-RULES.md; the short
version is that the proprietary Adreno compiler lowers indexed local storage to
private (scratch) memory and, in at least one measured case, read it back
*wrong*, so these are correctness checks and not style checks.

Checks
------
dynamic-local-array-index
    A function-scope array (or an ``out``/``inout`` array parameter) indexed by
    an expression that is not a compile-time constant.  On Adreno this becomes
    a private-memory array: hundreds of bytes of scratch per invocation, and
    the chroma-residual miscompile of commit d47c095.

loop-local-array-index
    The same, but the only non-constant part of the index is the counter of a
    ``for`` loop with constant bounds.  Safe *if* the loop is fully unrolled,
    which is not guaranteed once the trip count grows.  Reported separately so
    a genuinely dynamic index is never lost in the noise.

large-struct-by-value
    A struct larger than --struct-threshold scalar components passed to a
    function by value (no qualifier, or an explicit ``in``).  glslang copies it
    into a private-memory temporary, with the same cost as above.

subgroup-clustered
    ``subgroupClustered*`` in a normative shader.  paper 3.2.6 forbids them:
    "Cluster operations use subgroupBallot plus masks derived from
    gl_SubgroupInvocationID & ~7, never subgroupClustered*".

spv-unsafe-opt
    A CMake shader-build rule that runs ``glslc -O`` (whose built-in pass list
    contains the redundancy-elimination passes) or that names a
    redundancy-elimination pass explicitly.  That pass CSEs the OpAccessChains
    of a shared-memory load/store pair into one pointer id and the Adreno 650
    driver miscompiles the result.

Severity
--------
Every rule but one is an error: it fails the run.  ``loop-local-array-index`` is
an advisory, because a local array indexed only by a constant-bound loop counter
is correct as long as the unroller reaches it -- which it does today, and which
the SPIR-V pass list is built to keep doing.  Advisories are printed and counted
but do not set the exit status unless ``--strict`` is given.

Waivers
-------
Put ``nxvc-lint: allow <rule-id> -- <reason>`` in a comment on the offending
line or on the line above it.  A reason is mandatory.

Usage
-----
    scripts/shader-lint.py [--root DIR] [--format text|github] [paths...]

Exit status is 1 when any finding is reported, 0 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------
# What we look at.  The tourney worktrees carry their own vk/ trees that are
# snapshots of other people's branches, and the build directories carry copies
# of the sources; neither is ours to lint.
# --------------------------------------------------------------------------
SHADER_ROOTS = [
    "vk/decoder",
    "vk/encoder",
    "vk/common",
    "warp/glsl",
    "bench/shaders",
    "android",
]

CMAKE_SHADER_RULES = [
    "vk/common/cmake/NxvcEmbedShaders.cmake",
    "vk/common/cmake/NxvcShaderPasses.cmake",
    "bench/cmake/gen_spv.cmake",
    "vk/encoder/cmake/gen_spv.cmake",
    "vk/decoder/passA/cmake/gen_spv.cmake",
    "vk/decoder/passB/cmake/gen_spv.cmake",
    "android/cmake/gen_spv.cmake",
    "warp/CMakeLists.txt",
]

EXCLUDE_DIR_RE = re.compile(
    r"(^|/)(build[^/]*|\.cxx|CMakeFiles|_CPack_Packages|tourney[^/]*|node_modules)(/|$)"
)

# Shaders whose SPIR-V is part of the normative decode path.  bench/, android/
# and warp/ are tools and clients: subgroupClustered* is merely unwise there,
# not a spec violation.
NORMATIVE_PREFIXES = ("vk/decoder/", "vk/encoder/", "vk/common/")

# --------------------------------------------------------------------------
# GLSL scalar/vector/matrix types, and how many 32-bit-ish components each has.
# Used only to size structs; approximate on purpose.
# --------------------------------------------------------------------------
_SCALARS = {
    "bool": 1, "int": 1, "uint": 1, "float": 1, "double": 2,
    "int8_t": 1, "uint8_t": 1, "int16_t": 1, "uint16_t": 1,
    "int64_t": 2, "uint64_t": 2, "float16_t": 1, "float32_t": 1,
    "i8vec": 1, "u8vec": 1,
}
BASE_TYPES: dict[str, int] = dict(_SCALARS)
for _p, _n in (("", 1), ("i", 1), ("u", 1), ("b", 1), ("d", 2),
               ("i16", 1), ("u16", 1), ("i8", 1), ("u8", 1), ("f16", 1)):
    for _k in (2, 3, 4):
        BASE_TYPES[f"{_p}vec{_k}"] = _k * _n
for _c in (2, 3, 4):
    for _r in (2, 3, 4):
        BASE_TYPES[f"mat{_c}x{_r}"] = _c * _r
        BASE_TYPES[f"dmat{_c}x{_r}"] = 2 * _c * _r
    BASE_TYPES[f"mat{_c}"] = _c * _c
    BASE_TYPES[f"dmat{_c}"] = 2 * _c * _c

TYPE_QUALIFIERS = {
    "const", "in", "out", "inout", "highp", "mediump", "lowp", "precise",
    "flat", "smooth", "noperspective", "restrict", "coherent", "volatile",
    "readonly", "writeonly", "nonuniformEXT",
}

# Statement keywords that must never be mistaken for a declaration's type.
KEYWORDS = {
    "if", "for", "while", "do", "switch", "case", "default", "return",
    "break", "continue", "discard", "else", "struct", "layout", "shared",
    "uniform", "buffer", "subroutine", "barrier", "memoryBarrier",
}

# Rules that report but do not fail the run.  See "Severity" above.
ADVISORY_RULES = {"loop-local-array-index"}

IDENT_RE = re.compile(r"[A-Za-z_]\w*")
# Numeric literals with their GLSL suffixes and hex digits.  These must be
# removed before identifiers are harvested: otherwise the `u` of `0u` reads as an
# identifier, and every `for (uint k = 0u; ...)` counter looks like a runtime
# value, which turned every such loop into a false "dynamic index".
NUMBER_RE = re.compile(
    r"\b(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)[uUlLfF]*")


def free_identifiers(expr: str) -> set[str]:
    """Identifiers in `expr`, with numeric literals and type-cast spellings
    removed."""
    expr = NUMBER_RE.sub(" ", expr)
    expr = re.sub(r"\b(?:int|uint|u?int\d+_t|float|bool)\s*\(", "(", expr)
    return set(IDENT_RE.findall(expr))
INT_LIT_RE = re.compile(r"^(0[xX][0-9a-fA-F]+|\d+)[uU]?$")
WAIVER_RE = re.compile(r"nxvc-lint\s*:\s*allow\s+([\w-]+)\s*(?:--\s*(.*))?$")


@dataclass
class Finding:
    path: str
    line: int
    rule: str
    construct: str
    fix: str

    def __lt__(self, other: "Finding") -> bool:
        return (self.path, self.line, self.rule) < (other.path, other.line, other.rule)


# --------------------------------------------------------------------------
# Source preparation
# --------------------------------------------------------------------------
def strip_comments(src: str) -> str:
    """Blank out comments and string literals, keeping every byte offset and
    every newline, so offsets map straight back to line numbers."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = out[i + 1] = " "
                i += 2
        elif c in "\"'":
            quote = c
            out[i] = " "
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n and src[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def blank_directives(src: str) -> str:
    """Blank every preprocessor directive line, offsets preserved.

    Structural scanning needs this: a `#endif` sitting between one function's
    closing brace and the next function's return type hides that function (and
    every function after it) from FUNC_RE, which silently skipped whole files.
    The bodies inside #if blocks are deliberately left alone -- both arms of a
    conditional are worth checking, since either can be the one that ships."""
    out = []
    for line in src.split("\n"):
        out.append(" " * len(line) if line.lstrip().startswith("#") else line)
    return "\n".join(out)


def line_of(src: str, pos: int) -> int:
    return src.count("\n", 0, pos) + 1


def waived(raw_lines: list[str], line: int, rule: str) -> bool:
    for probe in (line, line - 1):
        if 1 <= probe <= len(raw_lines):
            m = WAIVER_RE.search(raw_lines[probe - 1])
            if m and m.group(1) == rule and (m.group(2) or "").strip():
                return True
    return False


def match_brace(src: str, open_pos: int) -> int:
    """Index of the '}' matching the '{' at open_pos, or len(src)."""
    depth = 0
    for i in range(open_pos, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(src)


def match_bracket(src: str, open_pos: int) -> int:
    depth = 0
    for i in range(open_pos, len(src)):
        if src[i] == "[":
            depth += 1
        elif src[i] == "]":
            depth -= 1
            if depth == 0:
                return i
    return -1


# --------------------------------------------------------------------------
# Constants visible to the shader
# --------------------------------------------------------------------------
def collect_constants(src: str, includes: list[str]) -> set[str]:
    """Names usable as a compile-time constant array index.

    Object-like #defines whose body is an integer constant expression, global
    ``const`` scalars, and specialization constants (which the driver resolves
    at pipeline creation, so an index built from one is still a constant
    OpAccessChain, not scratch)."""
    names: set[str] = set()
    for text in [src] + includes:
        for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.+)$",
                             text, re.M):
            body = m.group(2).strip()
            if _is_const_expr(body, names):
                names.add(m.group(1))
        for m in re.finditer(
            r"(?:^|;|\{|\})\s*(?:layout\s*\([^)]*\)\s*)?const\s+"
            r"(?:highp\s+|mediump\s+|lowp\s+)?(?:u?int|uint\d*_t|int\d*_t)\s+"
            r"([A-Za-z_]\w*)\s*=\s*([^;]+);", text):
            if _is_const_expr(m.group(2), names):
                names.add(m.group(1))
        for m in re.finditer(
            r"layout\s*\([^)]*constant_id[^)]*\)\s*const\s+\w+\s+([A-Za-z_]\w*)",
            text):
            names.add(m.group(1))
    return names


def _is_const_expr(expr: str, known: set[str]) -> bool:
    """True when every identifier in `expr` is already a known constant and the
    expression is made only of integer-arithmetic punctuation."""
    expr = expr.strip()
    if not expr:
        return False
    # A cast-like call such as int(FOO) or uint(3) is still constant.
    if free_identifiers(expr) - known:
        return False
    stripped = NUMBER_RE.sub(" ", expr)
    stripped = re.sub(r"\b(?:int|uint|u?int\d+_t)\s*\(", "(", stripped)
    without_idents = IDENT_RE.sub("", stripped)
    return bool(re.fullmatch(r"[\s\(\)\+\-\*/%<>&|\^~,\.]*", without_idents))


def classify_index(expr: str, consts: set[str], loop_vars: set[str]) -> str:
    """'const', 'loop' or 'dynamic'."""
    free = free_identifiers(expr) - consts
    if not free:
        return "const"
    if free <= loop_vars:
        return "loop"
    return "dynamic"


# --------------------------------------------------------------------------
# Structs
# --------------------------------------------------------------------------
def collect_structs(src: str, includes: list[str]) -> tuple[dict[str, int], set[str]]:
    """(struct name -> approximate component count, structs holding an array).

    The array set matters on its own: a struct with an array member cannot be
    scalarised into registers the way a handful of loose scalars can, so it is
    flagged whatever its size."""
    sizes: dict[str, int] = {}
    with_array: set[str] = set()
    pending: list[tuple[str, str]] = []
    for text in [src] + includes:
        for m in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{", text):
            end = match_brace(text, m.end() - 1)
            pending.append((m.group(1), text[m.end():end]))
    # Two passes so a struct made of other structs gets a real size.
    for name, body in pending:
        for field in body.split(";"):
            if re.search(r"\[[^\]]*\]", field):
                with_array.add(name)
                break
    for _ in range(3):
        for name, body in pending:
            total = 0
            for field in body.split(";"):
                field = field.strip()
                if not field:
                    continue
                toks = [t for t in re.split(r"\s+", field) if t not in TYPE_QUALIFIERS]
                if not toks:
                    continue
                base = toks[0]
                unit = BASE_TYPES.get(base, sizes.get(base, 1 if base in sizes else 0))
                if unit == 0:
                    unit = BASE_TYPES.get(base, 1)
                count = 1
                for dim in re.findall(r"\[([^\]]*)\]", field):
                    dim = dim.strip()
                    count *= int(dim) if dim.isdigit() else 1
                total += unit * count
            sizes[name] = total
            if name in with_array:
                continue
            # A struct made of structs inherits the array problem.
            for field in body.split(";"):
                toks = [x for x in re.split(r"\s+", field.strip())
                        if x not in TYPE_QUALIFIERS]
                if toks and toks[0] in with_array:
                    with_array.add(name)
                    break
    return sizes, with_array


# --------------------------------------------------------------------------
# Function bodies
# --------------------------------------------------------------------------
@dataclass
class Function:
    name: str
    params: str
    body_start: int   # index of '{'
    body_end: int     # index of '}'


FUNC_RE = re.compile(
    r"(?:^|[;\}])\s*(?:(?:const|highp|mediump|lowp|precise)\s+)*"
    r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\(([^;{)]*)\)\s*\{", re.S)


def find_functions(src: str) -> list[Function]:
    out = []
    for m in FUNC_RE.finditer(src):
        rtype, name, params = m.group(1), m.group(2), m.group(3)
        if rtype in KEYWORDS or name in KEYWORDS:
            continue
        open_pos = src.index("{", m.end() - 1)
        out.append(Function(name, params, open_pos, match_brace(src, open_pos)))
    return out


DECL_RE = re.compile(
    r"(?:^|[;\{\}])\s*((?:(?:const|highp|mediump|lowp|precise)\s+)*)"
    r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\[", re.M)

PARAM_ARRAY_RE = re.compile(
    r"\b(out|inout|in)\s+(?:(?:const|highp|mediump|lowp|precise)\s+)*"
    r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\[")

FOR_RE = re.compile(r"\bfor\s*\(")


def loop_vars_at(src: str, fn: Function, pos: int, consts: set[str]) -> set[str]:
    """Counters of constant-bound for loops whose body contains pos."""
    out: set[str] = set()
    for m in FOR_RE.finditer(src, fn.body_start, fn.body_end):
        paren = src.index("(", m.start())
        depth, close = 0, -1
        for i in range(paren, fn.body_end + 1):
            if src[i] == "(":
                depth += 1
            elif src[i] == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close < 0:
            continue
        header = src[paren + 1:close]
        parts = header.split(";")
        if len(parts) < 2:
            continue
        init = re.search(r"([A-Za-z_]\w*)\s*=\s*([^;]*)$", parts[0])
        if not init:
            continue
        var = init.group(1)
        cond = parts[1]
        bound = re.search(r"[<>]=?\s*(.+)$", cond)
        if not bound:
            continue
        if classify_index(init.group(2), consts, set()) != "const":
            continue
        if classify_index(bound.group(1), consts, set()) != "const":
            continue
        # Scope: the statement or block that follows the ')'.
        j = close + 1
        while j < fn.body_end and src[j] in " \t\r\n":
            j += 1
        if j < fn.body_end and src[j] == "{":
            end = match_brace(src, j)
        else:
            end = src.find(";", j)
            end = fn.body_end if end < 0 else end
        if close < pos <= end:
            out.add(var)
    return out


# --------------------------------------------------------------------------
# The shader check
# --------------------------------------------------------------------------
def resolve_includes(path: Path, root: Path) -> list[str]:
    texts = []
    try:
        raw = path.read_text(errors="replace")
    except OSError:
        return texts
    for m in re.finditer(r'^\s*#\s*include\s+["<]([^">]+)[">]', raw, re.M):
        for cand in (path.parent / m.group(1),
                     root / m.group(1),
                     path.parent / Path(m.group(1)).name):
            if cand.is_file():
                texts.append(strip_comments(cand.read_text(errors="replace")))
                break
    return texts


def check_shader(path: Path, root: Path) -> list[Finding]:
    rel = path.relative_to(root).as_posix()
    raw = path.read_text(errors="replace")
    raw_lines = raw.splitlines()
    src = strip_comments(raw)
    # Directives are needed to harvest #defines but get in the way of every
    # structural scan, so the two views are kept side by side.  Blanking
    # preserves byte offsets, so line numbers agree between them.
    code = blank_directives(src)
    includes = resolve_includes(path, root)
    consts = collect_constants(src, includes)
    structs, structs_with_array = collect_structs(src, includes)
    findings: list[Finding] = []

    def add(line: int, rule: str, construct: str, fix: str) -> None:
        if not waived(raw_lines, line, rule):
            findings.append(Finding(rel, line, rule, construct, fix))

    # ---- subgroupClustered* (normative shaders only) ---------------------
    if rel.startswith(NORMATIVE_PREFIXES):
        for m in re.finditer(r"\b(subgroupClustered\w*)\s*\(", code):
            line = line_of(code, m.start())
            add(line, "subgroup-clustered", f"{m.group(1)}()",
                "paper 3.2.6: build the cluster mask yourself -- "
                "subgroupBallot() masked with a mask derived from "
                "(gl_SubgroupInvocationID & ~7u), then subgroupShuffle/"
                "bitCount over it.")

    for fn in find_functions(code):
        body = code[fn.body_start:fn.body_end]
        # name -> (line, is_const, decl-text, offset of the declaration in body)
        arrays: dict[str, tuple[int, bool, str, int]] = {}

        # ---- out/inout (and by-value in) array parameters -----------------
        for m in PARAM_ARRAY_RE.finditer(fn.params):
            close = match_bracket(fn.params, m.end() - 1)
            decl = fn.params[m.start():close + 1].strip()
            arrays[m.group(3)] = (line_of(code, fn.body_start), False, decl, -1)

        # ---- function-scope array declarations ---------------------------
        for m in DECL_RE.finditer(body):
            quals, typ, name = m.group(1), m.group(2), m.group(3)
            if typ in KEYWORDS or typ in TYPE_QUALIFIERS:
                continue
            if typ not in BASE_TYPES and typ not in structs:
                continue
            close = match_bracket(body, m.end() - 1)
            if close < 0:
                continue
            decl = re.sub(r"\s+", " ", body[m.start():close + 1]).strip(" ;{}")
            arrays[name] = (line_of(code, fn.body_start + m.start(1)),
                            "const" in quals.split(), decl, m.start(3))

        # ---- every use of those names ------------------------------------
        for name, (_dline, is_const, decl, decl_off) in arrays.items():
            for m in re.finditer(rf"\b{re.escape(name)}\s*\[", body):
                close = match_bracket(body, m.end() - 1)
                if close < 0:
                    continue
                if m.start() == decl_off:
                    continue          # the declaration's own size, not an index
                expr = body[m.end():close]
                abspos = fn.body_start + m.start()
                kind = classify_index(expr, consts, loop_vars_at(code, fn, abspos, consts))
                if kind == "const":
                    continue
                line = line_of(code, abspos)
                what = "const local array" if is_const else "local array"
                if kind == "dynamic":
                    add(line, "dynamic-local-array-index",
                        f"{what} `{decl}` indexed by non-constant "
                        f"`{expr.strip()}` (in {fn.name}())",
                        _fix_for(is_const, name))
                else:
                    add(line, "loop-local-array-index",
                        f"{what} `{decl}` indexed by loop counter "
                        f"`{expr.strip()}` (in {fn.name}())",
                        "Safe only while the loop is fully unrolled.  Either "
                        "keep the trip count small and add "
                        "`// nxvc-lint: allow loop-local-array-index -- <why>`, "
                        "or apply the rewrite for a dynamic index.")

        # ---- whole-struct copies out of a buffer -------------------------
        # `TileParam p = params[i];` reads like a cheap alias and is not: it
        # materialises the entire record in a Function-storage temporary.  Only
        # this shape is flagged, not every local struct -- a record assembled
        # field by field and stored once is scalarised normally.
        for m in re.finditer(
                r"(?:^|[;\{\}])\s*(?:const\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)"
                r"\s*=\s*([A-Za-z_]\w*(?:\.\w+)*)\s*\[", body):
            typ, var, srcname = m.group(1), m.group(2), m.group(3)
            if typ not in structs:
                continue
            if structs[typ] <= STRUCT_THRESHOLD[0] and typ not in structs_with_array:
                continue
            add(line_of(code, fn.body_start + m.start(1)), "large-struct-by-value",
                f"`{typ} {var} = {srcname}[...]` copies the whole "
                f"{typ} record (~{structs[typ]} components"
                f"{', array member' if typ in structs_with_array else ''}) "
                f"into a local (in {fn.name}())",
                f"Read the fields you need directly -- `{srcname}[i].field` -- "
                f"instead of copying the record.  Each of those is a plain load "
                f"with a constant member index; the copy is a private-memory "
                f"temporary on Adreno.  See docs/ADRENO-RULES.md.")

        # ---- large structs by value --------------------------------------
        for p in _split_params(fn.params):
            pm = re.match(
                r"^\s*((?:(?:const|in|highp|mediump|lowp|precise)\s+)*)"
                r"([A-Za-z_]\w*)\s+([A-Za-z_]\w*)", p)
            if not pm:
                continue
            if re.search(r"\b(out|inout)\b", p):
                continue
            typ = pm.group(2)
            if typ not in structs:
                continue
            size = structs[typ]
            if size <= STRUCT_THRESHOLD[0] and typ not in structs_with_array:
                continue
            add(line_of(code, fn.body_start), "large-struct-by-value",
                f"parameter `{p.strip()}` passes struct {typ} "
                f"(~{size} components"
                f"{', array member' if typ in structs_with_array else ''}) "
                f"by value to {fn.name}()",
                f"Take it as `in {typ}` only if glslang can keep it in "
                f"registers; otherwise pass the few fields actually read as "
                f"scalars, or make the parameter a buffer index and read the "
                f"fields from the SSBO.  A by-value struct copy lands in "
                f"private memory on Adreno.")

    return findings


STRUCT_THRESHOLD = [8]


def _split_params(params: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def _fix_for(is_const: bool, name: str) -> str:
    if is_const:
        return (f"`{name}` is a read-only table: bit-pack it into one or more "
                f"`uint` constants and extract with a shift+mask, or move it to "
                f"a `shared` array filled once per workgroup.  A const array "
                f"with a dynamic index still becomes a Private variable.")
    return (f"Replace `{name}` with either (a) explicit scalars plus a "
            f"switch/if-chain or a select ladder so every access chain is a "
            f"compile-time constant, (b) a `shared` array indexed by "
            f"gl_LocalInvocationID (LDS is addressable and correct on Adreno), "
            f"or (c) an SSBO scratch slice.  See docs/ADRENO-RULES.md.")


# --------------------------------------------------------------------------
# CMake shader-build rules
# --------------------------------------------------------------------------
UNSAFE_PASSES = ("--redundancy-elimination", "--local-redundancy-elimination")


def check_cmake(path: Path, root: Path) -> list[Finding]:
    rel = path.relative_to(root).as_posix()
    raw = path.read_text(errors="replace")
    raw_lines = raw.splitlines()
    findings: list[Finding] = []

    def add(line: int, construct: str, fix: str) -> None:
        if not waived(raw_lines, line, "spv-unsafe-opt"):
            findings.append(Finding(rel, line, "spv-unsafe-opt", construct, fix))

    # Comments in CMake start at '#'; blank them so a mention of -O in prose
    # does not trip the check.
    code_lines = [re.sub(r"#.*$", "", ln) for ln in raw_lines]

    for i, ln in enumerate(code_lines, 1):
        if not re.search(r"\$\{\w*GLSLC\w*\}|\bglslc\b|SHADER_COMPILER", ln) and \
           not re.search(r"^\s*(--target-env|-O\b)", ln):
            # -O may sit on a continuation line of a glslc command; look back.
            pass
        m = re.search(r"(?<![\w-])-O[s0-9]?(?![\w=])", ln)
        if not m:
            continue
        flag = m.group(0)
        if flag in ("-O0",):
            continue
        # Is this line part of a glslc invocation?
        window = " ".join(code_lines[max(0, i - 6):i + 2])
        if not re.search(r"\$\{\w*GLSLC\w*\}|\bglslc\b|SHADER_COMPILER|-fshader-stage",
                         window):
            continue
        add(i, f"glslc invoked with `{flag}`",
            "glslc's -O runs spirv-opt's full -O list, which includes "
            "--redundancy-elimination and --local-redundancy-elimination.  "
            "Compile with -O0 and run spirv-opt explicitly with the shared "
            "safe pass list (nxvc_spirv_safe_passes / NXVC_SPIRV_SAFE_PASSES "
            "in vk/common/cmake/NxvcShaderPasses.cmake).")

    for i, ln in enumerate(code_lines, 1):
        for bad in UNSAFE_PASSES:
            if bad in ln:
                add(i, f"spirv-opt pass `{bad}` in the pass list",
                    "Drop it.  It CSEs the OpAccessChain pair of a shared-memory "
                    "load/store into one pointer id and the Adreno 650 driver "
                    "returns the wrong word.  See docs/ADRENO-RULES.md.")
    return findings


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def gather(root: Path, explicit: list[str]) -> tuple[list[Path], list[Path]]:
    shaders: list[Path] = []
    cmakes: list[Path] = []
    if explicit:
        for p in explicit:
            path = Path(p)
            if not path.is_absolute():
                path = root / path
            if path.suffix in (".comp", ".glsl"):
                shaders.append(path)
            else:
                cmakes.append(path)
        return sorted(set(shaders)), sorted(set(cmakes))

    for r in SHADER_ROOTS:
        base = root / r
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            relative = Path(dirpath).relative_to(root).as_posix()
            dirnames[:] = [
                d for d in dirnames
                if not EXCLUDE_DIR_RE.search(f"{relative}/{d}")
                and not d.startswith(".")
            ]
            if EXCLUDE_DIR_RE.search(relative):
                continue
            for fn in filenames:
                if fn.endswith((".comp", ".glsl")):
                    shaders.append(Path(dirpath) / fn)
    for c in CMAKE_SHADER_RULES:
        if (root / c).is_file():
            cmakes.append(root / c)
    return sorted(set(shaders)), sorted(set(cmakes))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", help="files to check (default: the tree)")
    ap.add_argument("--root", default=None, help="repository root")
    ap.add_argument("--format", choices=("text", "github"), default="text")
    ap.add_argument("--struct-threshold", type=int, default=8,
                    help="components above which a by-value struct is flagged; a struct with an array member is flagged at any size")
    ap.add_argument("--list-files", action="store_true",
                    help="print what would be checked and exit")
    ap.add_argument("--strict", action="store_true",
                    help="fail on advisories too (loop-local-array-index)")
    args = ap.parse_args(argv)

    STRUCT_THRESHOLD[0] = args.struct_threshold
    root = Path(args.root).resolve() if args.root else \
        Path(__file__).resolve().parent.parent
    shaders, cmakes = gather(root, args.paths)

    if args.list_files:
        for p in shaders + cmakes:
            print(p.relative_to(root).as_posix())
        return 0

    findings: list[Finding] = []
    for p in shaders:
        try:
            findings += check_shader(p, root)
        except Exception as exc:  # a parse failure must not hide the others
            print(f"shader-lint: {p}: internal error: {exc}", file=sys.stderr)
            return 2
    for p in cmakes:
        findings += check_cmake(p, root)

    findings.sort()
    def severity(f: Finding) -> str:
        return "advice" if f.rule in ADVISORY_RULES else "error"

    if args.format == "github":
        for f in findings:
            kind = "notice" if severity(f) == "advice" else "warning"
            print(f"::{kind} file={f.path},line={f.line}::"
                  f"[{f.rule}] {f.construct} | fix: {f.fix}")
    else:
        for f in findings:
            print(f"{f.path}:{f.line}: {severity(f)}: {f.rule}: {f.construct}")
            print(f"    fix: {f.fix}")

    counts: dict[str, int] = {}
    for f in findings:
        counts[f.rule] = counts.get(f.rule, 0) + 1
    hard = [f for f in findings if severity(f) == "error"]
    print(f"\nshader-lint: {len(findings)} finding(s) "
          f"({len(hard)} error, {len(findings) - len(hard)} advisory) over "
          f"{len(shaders)} shader(s) and {len(cmakes)} build rule(s)",
          file=sys.stderr)
    for rule in sorted(counts):
        mark = " (advisory)" if rule in ADVISORY_RULES else ""
        print(f"  {rule}: {counts[rule]}{mark}", file=sys.stderr)
    failing = findings if args.strict else hard
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
