# Plan: Harden the GLSL/Cg preprocessor + FFP shader build against hard crashes

**Request:** `~/.agents/projects/vitaGL/requests/2026-06-08-ffp-preprocessor-crash.md`
**Branch:** `matt.spurlin/ffp-preprocessor-crash` (off `matt.spurlin/cdram-display-memblocks`, HEAD `f92c104`)
**Goal:** Make shader preprocessing/compilation **fail safe (return a GL error), never abort the process.**

> **Revision note.** This plan was reviewed by two independent Opus reviewers. Both
> **fully confirmed** the root-cause analysis (all line numbers, the FFP/preprocessor
> de-unification, the dead-`writeTok` nearest-symbol argument). Their corrections are
> folded in: the error channel is re-pointed at **link** status (default mode is
> POSTPONED), a **new leading FFP-crash candidate** (NULL shark result deref) is added as
> Bug F, the output-buffer contract is changed to **size-from-result**, and confidence
> language is tempered. Changes vs. the first draft are marked `[rev]`.

---

## TL;DR for reviewers / implementers

The request asserts *"two independent triggers (FFP + `#version 300 es`) land in the same
preprocessor function."* **That unified framing is wrong for the FFP case** — verified,
not assumed:

- `glsl_preprocess` (vitaGL's own preprocessor) has **exactly one caller**:
  `glsl_utils.c:1090`, inside `glsl_translator_process`, reached only via the
  **custom-shader** path (compile: `custom_shaders.c:1778`; link:
  `glsl_utils.c:1377/1380` ← `custom_shaders.c:2167`).
- The **FFP** path (`ffp.c`) builds `ffp_vert_src`/`ffp_frag_src` with `sprintf` and hands
  the Cg **directly** to `shark_compile_shader_extended` (SceShaccCg). It **never touches
  `preprocessor.cpp`.**
- `writeTok` (the reported FFP PC) is **dead code** (only call site is in a `/* */` block,
  `:704–736`). `PC→writeTok:553` is **nearest-symbol resolution** against a binary that
  wasn't `--gc-sections`'d; the real FFP frame is elsewhere in vitaGL's static range.

| Reported crash | Real path | Confidence | Fixed by |
|---|---|---|---|
| boxy `#version 300 es`, "ok=true then crash at `glLinkProgram`" | custom-shader → `preprocessor.cpp`, **at link time** (POSTPONED default) | **Medium-High** `[rev]` — real memory-safety bugs on the exact path; confirm via ASan | Bugs **A**, **B** + Step 2 error channel |
| inputty FFP, native abort, `PC→writeTok:553` | FFP → `shark`/SceShaccCg, **bypasses preprocessor** | **Low** (symbol mis-resolution) — but Bug **F** is a concrete, host-verifiable candidate | Bug **F** (leading), **D**, **E** defensive |

**Two facts every reviewer must keep straight:**
1. **Default semantic mode is `VGL_MODE_POSTPONED`** (`glsl_utils.c:112`). In that mode
   `glCompileShader` **returns before preprocessing** (`custom_shaders.c:1756-1757`) and
   `glGetShaderiv(GL_COMPILE_STATUS)` is **`GL_TRUE` unconditionally**
   (`custom_shaders.c:1631-1633`) — this *is* boxy's "ok=true". The preprocessor runs in
   **`glLinkProgram`**. So the failure channel is **`glGetProgramiv(GL_LINK_STATUS)` /
   `p->status`**, not compile status.
2. The new **Step 1 `tokName()` bounds helper throws**, and that is only safe **because
   Step 1's boundary `try/catch` ships in the same change.** Helper-without-catch would
   just relocate the abort. They land together.

---

## Root-cause findings (all line-verified; both reviewers confirmed)

### Bug A — Heap buffer overflow in the preprocessor output copy `[HIGH, boxy] [segfault — NOT caught by try/catch]`
- `glsl_utils.c:1089`: `char *out = vglMalloc(strlen(input));` — **no `+1`**, no expansion
  headroom.
- `preprocessor.cpp:2145`: `strcpy(output, out.c_str());` — unbounded copy of the full
  expanded string. Identity transforms still overflow by the NUL byte; macro expansion
  overflows by more. `glsl_hdr`/`glsl_ffp_hdr` are `strcat`'d into a *separately*-sized
  `s->source` later (`glsl_utils.c:1166-1201`, correctly counted into `size`), so the
  overflow is **purely `input`→`out` expansion**. Heap corruption → deferred crash
  (boxy's link-time death). **This is a memory bug; a `try/catch` does not catch it — only
  the bounded copy fixes it.** `[rev: A and B are separate fixes]`

### Bug B — Uncaught C++ exception across the `extern "C"` boundary `[HIGH, boxy] [abort — fixed by try/catch]`
- Throw sites (all `std::string`): `wrtError:167`, `getTok:457`, `findIncludeFile:1021`,
  `preprocess:1958`, and `expression.cpp:47/58/67/415/417/435/618` (reached via
  `expression::evaluate` at `processLine:1650/1714`).
- `glsl_preprocess` (`preprocessor.cpp:2138`) is `extern "C"` with **no try/catch**, nor
  any caller up the chain. Unwinding past `extern "C"` → `std::terminate()` → `abort()` =
  the request's "native abort". A boundary `catch (const std::string&)` (+ `catch(...)`
  backstop for `bad_alloc` etc.) covers the entire tree.

### Bug C — Unchecked `tokNames[tok.type]` indexing `[latent hardening]`
- `tokNames[]` = **65 entries** (indices `0..PRAGMA(64)`; counted + `const.h` enum). Token
  types run to `NOEXPAND(94)`.
- Unchecked indexers: `writeTok:565`, `tok2Str:607` (**live** — `g_outfile==NULL` for
  `glsl_preprocess`, so `writeLine:695`→`tok2Str`), `processLine:1494`. `[rev]` also note
  `names[tok.type]` at `:1892` (lex-mode/debug, dead for our "full" path, full-size array
  — low risk; route it through the same helper for completeness).

### Bug D — FFP path ignores compiler-init failure `[defensive, secondary]`
- `ffp.c:686-687` (vertex) / `915-916` (fragment) drop `start_shader_compiler()`'s return,
  then unconditionally compile. Custom path guards correctly (`custom_shaders.c:1748`,
  where `SET_GL_ERROR` → `return;`). `libshacccg.suprx` *is* present on the test device, so
  this is **not** the confirmed FFP cause — defensive only.

### Bug E — FFP `sprintf` into fixed stack buffers + overlapping-buffer UB `[defensive; not the minimal-repro cause]`
- `char vshader[8192]`, `fshader[8192]`, `texenv_shad[8192]`. Minimal no-texture
  `fshader` ≈5.5 KB (template `ffp_f.h` = 5669 B, empty `%s`) **fits 8192** → stack
  overflow is **not** the minimal-repro crash. Overflows only once textures append.
- **UB:** `ffp.c:929/935/941/947/953/960` `sprintf(texenv_shad,"%s\n%s",texenv_shad,src)`
  reads and writes `texenv_shad` simultaneously (overlapping source/dest). Only fires with
  textures; real but not the minimal repro.

### Bug F — `[rev] [NEW — leading host-verifiable FFP-crash candidate]` FFP derefs a NULL shark result
- `ffp.c:695-698` (vertex) / `984-987` (fragment): the `if (t) { ... }` guard on the
  compiler output `t` is **inside `#ifdef DUMP_SHADER_SOURCES`**. In a normal build
  (`DUMP_SHADER_SOURCES` undefined) the guard **compiles out**, so when
  `shark_compile_shader_extended` returns **NULL** (compiler rejects the macro-heavy FFP
  Cg, or compiler not up), the code executes
  `vgl_fast_memcpy(ffp_*_program, (void*)t /*NULL*/, size)` (`:698/987`) and then registers
  `ffp_*_program` with the patcher (`:719`). **A NULL/garbage deref entirely inside
  vitaGL's static range — exactly where the mis-symbolicated FFP PC points.** This is the
  most plausible FFP crash that is *fixable from source*: if SceShaccCg rejects the FFP Cg,
  every first-FFP-draw dies here. **Fix: unconditional `if (!t) { bail safely }` before the
  memcpy/register, independent of `DUMP_SHADER_SOURCES`.**

### Alternative FFP hypothesis (flag, host-verify cheaply, don't chase)
This branch's distinguishing change is `f92c104` (dedicated-CDRAM display allocator); the
breadcrumb `vglInit ok=0` confirms it's live. The FFP abort *could* be heap corruption from
that allocator rather than the shader path. **Cheapest discriminator `[rev]`:** re-run the
FFP repro on the **parent commit** (before `f92c104`). Still crashes → not the allocator
(Bug F / shark the likelier cause); clean → allocator is implicated. This beats waiting on
an on-device backtrace as the first triage step.

---

## Implementation

High-confidence, symptom-matching fixes (A, B, Step 2, F) first.

### Step 1 — Bound the preprocessor output copy + catch at the boundary (Bugs A, B, C)  `[primary]`
Files: `source/utils/preprocessor/preprocessor.cpp` (`glsl_preprocess` `:2137-2147`,
`tok2Str`/`writeTok`/`processLine`), `source/utils/preprocessor/preprocessor_c.h`,
`source/utils/glsl_utils.c` (`:1089-1090`).

1. **`tokName()` bounds helper (Bug C):**
   ```cpp
   static const char *tokName(int t) {
       if (t < 0 || t >= (int)(sizeof(tokNames)/sizeof(tokNames[0])))
           wrtError("internal: token type out of range");   // throws -> caught at boundary (step 1.3)
       return tokNames[t];
   }
   ```
   Route `writeTok:565`, `tok2Str:607`, `processLine:1494`, and `names[]:1892` through a
   bounds check. Add the `g_outfile == NULL` guard in `writeTok` (defensive; it's dead
   code, but must never `fprintf(NULL)` if revived).
2. **Size-from-result contract (Bug A) `[rev: mandate, not heuristic]`.** Change
   `glsl_preprocess` so the destination is sized from the **actual** result length, not a
   guessed multiplier (macro nesting expands super-linearly — no fixed multiple is a safe
   upper bound). Preferred signature:
   ```c
   // Allocates *output (caller frees via vgl_free) sized to the exact result; returns
   // bytes written (excl NUL), or -1 on error. *output left NULL/empty on error.
   int glsl_preprocess(const char *mode, const char *infile, char **output);
   ```
   (If retaining a caller-owned fixed buffer is preferred for allocator reasons, the copy
   **must** be capacity-bounded and **any** truncation returns `-1` — never feed a
   truncated shader to shark. Size-from-result is the mandate; bounded-copy is only the
   memory-safety fallback.) Update the prototype in `preprocessor_c.h` (also drop the
   misleading non-`const` `char *mode`).
3. **Boundary try/catch (Bug B).** Wrap the `glsl_preprocess` body:
   `try { auto out = preprocessor::preprocess(...); /* alloc+copy out.size() */ }
   catch (const std::string &e) { vgl_log(...); return -1; }
   catch (...) { vgl_log(...); return -1; }`. No throw escapes `extern "C"`.

### Step 2 — Surface failure through the LINK channel (Bug B caller side) `[rev — primary, rewritten]`
Files: `source/utils/glsl_utils.c`, `source/custom_shaders.c`.

The default mode is **POSTPONED**, so preprocessing runs at **link**, and compile status is
already (wrongly) `GL_TRUE`. Therefore:

1. **Give `glsl_translator_process` an error return.** It is currently `void`
   (`glsl_utils.c:1049`). Make it return success/failure (or set a status field on `s`).
   On `glsl_preprocess` returning `-1`, **bail before** the destructive
   `vgl_free(s->source)` / `s->source = vglMalloc(...)` reallocation (`glsl_utils.c:1160-1161`)
   so the shader is left with `s->prog == NULL` and a clean source state.
2. **Check it at all three translation sites:** `glsl_utils.c:1377` and `:1380`
   (link-time, the default path) and `custom_shaders.c:1778` (compile-time, non-postponed
   modes).
3. **Gate link success on real programs.** `glLinkProgram` sets `p->status = PROG_LINKED`
   **unconditionally at `custom_shaders.c:2200`**, even when `p->vshader->prog` /
   `p->fshader->prog` are NULL. Gate that assignment on both shader progs being non-NULL,
   so `glGetProgramiv(GL_LINK_STATUS)` (reads `p->status == PROG_LINKED`,
   `custom_shaders.c:2057`) correctly reports failure. Populate the program info log.
4. **Link-time compiler guard.** The `glCompileShader` shark guard
   (`custom_shaders.c:1748`) doesn't protect the POSTPONED path where compilation happens
   in `glLinkProgram`. Add the `if (!is_shark_online && !start_shader_compiler())` guard at
   the link-time compile sites too.

### Step 3 — FFP NULL-result guard (Bug F)  `[rev — primary for the FFP case]`
File: `source/ffp.c` (`:695-698` vertex, `:984-987` fragment).

Add an **unconditional** check, independent of `DUMP_SHADER_SOURCES`:
```c
SceGxmProgram *t = shark_compile_shader_extended(...);
if (!t) {
    // compiler rejected the FFP source or is unavailable: bail this draw safely.
    // Leave ffp_*_program unset; do NOT memcpy/register a NULL program.
    // (mirror Bug D bail; verify the surrounding draw-setup tolerates a skipped program)
}
```
This removes the every-failure NULL deref and is the concrete, source-level FFP fix.

### Step 4 — FFP compiler-init guard (Bug D)  `[secondary, defensive]`
`ffp.c:686-687` / `915-916`: honor `start_shader_compiler()`'s return; if it fails, take
the same safe bail as Step 3 instead of compiling. Verify the early-out leaves
`ffp_vertex_program`/`ffp_fragment_program` in a state the draw path tolerates.

### Step 5 — Bound FFP source assembly (Bug E)  `[secondary, defensive]`
`source/ffp.c`: replace `sprintf(vshader,...)`/`sprintf(fshader,...)` with `snprintf`
bounded by `sizeof(...)`, treat truncation (`>= sizeof`) as a build failure (Step 3 bail).
Fix the overlapping `texenv_shad` append with an offset form:
`len += snprintf(texenv_shad+len, sizeof(texenv_shad)-len, "\n%s", src);` (guard `len`).

---

## Verification

1. **Build** the changed TUs clean (full Vita toolchain build if available; otherwise at
   minimum compile/type-check `preprocessor.cpp` + changed C TUs and **say which was run**).
   No "works" claim on compile alone.
2. **Host preprocessor harness (ASan) — exercises A/B/C directly.** Link
   `preprocessor.cpp` + `expression.*` into a tiny host program; feed: (a) a normal shader;
   (b) a malformed one (unterminated string / stray token) → must **return -1, not abort**;
   (c) a macro-heavy input whose expansion exceeds input length → **no overflow** under
   ASan. This is the cleanest confirmation that A/B are truly fixed (and, per reviewer 2,
   the honest way to *confirm* boxy rather than assume the symbolication).
3. **Confidence-honest hand-back** (below). Recommend the reporter (a) re-run the FFP repro
   on the parent of `f92c104` to triage the CDRAM-allocator hypothesis, and (b) capture a
   `--gc-sections`/`-fno-omit-frame-pointer` on-device backtrace so the FFP frame stops
   mis-resolving to dead `writeTok`.

## Residual risks / notes (from review)
- **Non-reentrant preprocessor globals** (`g_outfile`, `g_outstring`, `g_mode`,
  `g_lineno`, `g_fname`, `g_blacklist`, `g_stacks`) are serialized only by `THREAD_SAFE()`
  on the GL entry points — state that reliance; out of scope to refactor here.
- **`expand` recursion** (`preprocessor.cpp:755→863/956`) is bounded against macro
  re-expansion by `used`, but pathological nesting can still exhaust the Vita stack —
  out of scope, noted.

## Hand-back to reporter
- ✅ boxy `300 es`: preprocessor no longer overflows or aborts; malformed/over-large
  shaders now fail the **link** with `GL_LINK_STATUS == GL_FALSE` + info log
  (memory-safety bugs confirmed via ASan; symbolication-independent).
- ✅ inputty FFP: **Bug F** removes the NULL-result deref that crashes on every FFP compile
  failure (leading source-level candidate); FFP path additionally hardened against
  compiler-init failure and buffer overflow. ⚠️ The exact reported abort frame is
  unverified on host — request a symbol-accurate backtrace and a parent-commit re-run to
  rule the CDRAM allocator in or out.
</content>
