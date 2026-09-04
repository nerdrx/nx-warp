# NX Warp — Specification, version 1 (draft)

This directory is the **umbrella specification** for the NX Warp codec: the
document set that a second, independent implementer would read in order to
build a conforming decoder. It is written in the style of the AV1 and H.264
specifications: numbered clauses, syntax tables in descriptor form, semantics
per syntax element, and an explicitly ordered decoding process.

**Status: draft. Not yet a complete specification.** Every place where the
normative behaviour is not yet fixed carries a `[pending ...]` marker naming the
component document that will fix it. `spec/tools/check_spec.py` counts those
markers and cross-checks clause 04 against clause 05; the count is the honest
measure of how far from finished this document set is.

## Document map

| Clause | File | Contents |
|---|---|---|
| 0 | [`00-scope.md`](00-scope.md) | Scope, what this specification does and does not define |
| 1 | [`01-references.md`](01-references.md) | Normative and informative references |
| 2 | [`02-definitions.md`](02-definitions.md) | Terms, definitions, abbreviations, symbols |
| 3 | [`03-conventions.md`](03-conventions.md) | Arithmetic, integer semantics, Q-formats, bit-reading functions, descriptors |
| 4 | [`04-bitstream-syntax.md`](04-bitstream-syntax.md) | Syntax tables |
| 5 | [`05-semantics.md`](05-semantics.md) | Semantics of every syntax element |
| 6 | [`06-decoding-process.md`](06-decoding-process.md) | The decoding process, in execution order |
| 7 | [`07-transport.md`](07-transport.md) | The transport facts a decoder must know |
| 8 | [`08-profiles-levels.md`](08-profiles-levels.md) | Profiles, levels, capability bits |
| 9 | [`09-conformance.md`](09-conformance.md) | Conformance definition and vectors |
| Annex A | [`annex-a-tables.md`](annex-a-tables.md) | Normative constant tables |
| Annex B | [`annex-b-patents.md`](annex-b-patents.md) | Intellectual-property hygiene record (informative) |
| Annex C | [`annex-c-open-issues.md`](annex-c-open-issues.md) | Open issues, gaps and inter-document conflicts |

## Source documents and their standing

This specification does not invent codec behaviour. It transcribes, organises
and cross-checks the following, and cites each statement to its source:

| Document | Standing here | Covers |
|---|---|---|
| `docs/SYNTAX.md` | **normative source** | Bitstream syntax and the intra decoding process |
| `docs/TRANSPORT.md` | **normative source** | Wire format, receiver state, reference eligibility |
| `ref/` (CPU reference codec) | **normative source** | Constant tables, tie-breaks; `docs/SYNTAX.md` §0 makes it co-normative |
| `docs/STEREO.md` | **normative source** | Inter-view prediction (clause 6.8). Landed during drafting |
| `docs/ERRATA.md` | **normative source** | Corrections to the paper. Note that it and `docs/SYNTAX.md` currently disagree — Annex C issue C-20 |
| `docs/INTEGRATION-DECISIONS.md` | normative source | Origin of the `color_space` element |
| `warp/` (integer warp implementation) | provisional source | Awaiting `docs/WARP.md`; cited by file and commit |
| `docs/WARP.md` | *(does not exist yet)* | Integer pose warp — clause 6.7 is `[pending WARP.md]` |
| `docs/RATECONTROL.md` | *(does not exist yet)* | Encoder side, informative; no decoder normative content expected |
| `docs/HYBRID.md` | *(does not exist yet)* | HEVC base layer — clause 6.9 is `[pending HYBRID.md]` |
| `docs/PAPER.md` | **informative only** | Design rationale. Where the paper and a component document disagree, the component document wins and the disagreement is logged in Annex C |

Where a component document has not landed, the clause states what the paper
says, marks the field or step `[pending <DOC>]`, and Annex C records it. A
`[pending]` marker is a promise that no implementer should guess here — it is
never a claim that the behaviour is optional.

## Editorial conventions

1. **Key words.** MUST, MUST NOT, SHOULD, SHOULD NOT, MAY are to be read as in
   RFC 2119 / RFC 8174 (`01-references.md` [R-1]). "The decoder must reject"
   means the decode fails with an error, no output is produced, and no read
   occurs outside the supplied buffer.
2. **Normative vs informative.** Everything is normative unless a clause,
   sub-clause or table is marked *(informative)*. Encoder behaviour is
   informative throughout except where it constrains what a conforming
   bitstream may contain.
3. **Citations.** A statement transcribed from a source document is followed by
   its origin in square brackets, e.g. `[SYNTAX 6.5]`, `[TRANSPORT 9]`,
   `[PAPER 4.4]`, `[ref/src/tables.cpp @ d43182b]`. A citation to `ref/` or
   `warp/` always names the commit the transcription was made from.
4. **Pending markers.** `[pending SYNTAX.md]`, `[pending WARP.md]`,
   `[pending STEREO.md]`, `[pending HYBRID.md]`, `[pending review]` and
   `[pending citation]`. Each marker is one unresolved item and is counted by
   the checker. Do not remove a marker without replacing it with the fact.
5. **Syntax tables.** Clause 04 declares syntax elements in fenced blocks
   labelled ` ```syntax `, one element per line, in the classic descriptor
   style:

   ```
   name_of_element                                    f(8)
   ```

   The descriptors are defined in clause 3.6. Every identifier declared in such
   a block MUST have a definition in clause 05.
6. **Semantics entries.** Clause 05 defines each element with a paragraph that
   begins with the element name in bold backticks:

   ```
   **`base_qp`** is the frame-level quantisation parameter ...
   ```

   This is the form the checker looks for. Do not introduce a second style.
7. **Integer-only rule.** Clause 3.3 states the normative arithmetic rule for
   the whole document set: int32, no floating point, no division, no int64 on
   any per-sample or per-coefficient path. A clause that appears to need one of
   those is a bug in the clause.
8. **Numbers.** Hexadecimal is written `0x1234`. Binary is written `0b1010`.
   `2^n` is exponentiation, never XOR.

## Checking this document set

```sh
python3 spec/tools/check_spec.py spec        # human-readable report
python3 spec/tools/check_spec.py --strict spec   # nonzero exit if issues remain
ctest -R spec.check                          # the same, wired into the build
```

The `spec.check` test is registered by `tests/spec/CMakeLists.txt`, which the
root `CMakeLists.txt` picks up automatically. It is a *report*, not a gate: it
passes while `[pending]` markers remain, because they are the current honest
state of the document. `--strict` is what a release would use.
