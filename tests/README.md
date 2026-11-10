# Native ownership regression

On macOS, with the pinned submodules initialized and CMake/Clang available:

```bash
bash tests/run_native_ownership.sh "$PWD" /absolute/path/outside/repository/ownership-build
```

The runner compiles the actual production translation unit (including its
dispatcher and internal helpers) and the **full unchanged** pinned
`4DPluginAPI.c`, not copied API implementations. It builds unsigned Debug static
backend dependencies and writes all output to the supplied directory. The host
shim intercepts engine selectors only. ASan/UBSan are enabled; macOS LeakSanitizer
is unavailable, so every test also asserts explicit string, handle and
object/collection reference balances. Engine charset conversion returns bytes,
with byte capacities and no counted terminator; production code converts UTF-16
bytes to code units. Packed SDK fields are copied before binding STL references.

Named regressions `failed-property-output` and `failed-element-output` inject
native error -1234 with poisoned, non-owned output. Both require error 401,
property/index context, the native error, and no poisoned-output cleanup.
`successful-ownership` checks exact ASCII, BMP, supplementary and empty text,
allocated/null empties, replacement, independent getter copies, absent and
wrong-type properties, attachment parsing and mismatches, status warning
handoffs surviving parent disposal, object return handoff, array independent
transfers including index zero, rejected types/indices/null destinations,
replacement, shrink and final clear. Arrays are SDK contract checks, not an
assertion that this plugin uses text arrays.

To compare an immutable baseline, extract it with `git archive` into a separate
directory and pass that directory as the first argument to the **same final**
runner. Dependencies and harness remain the inspected worktree versions.
Baseline `d6b5e0655f99050e6a060fe82fa3ac62a751bfbc` fails both named
failed-output tests; the fixed source passes all three tests.

## Bounded inventory and contracts

The complete tracked first-party native inventory at that baseline is
`facturx/facturx-4dplugin.cpp` and `facturx/facturx-4dplugin.h`. CMake compiles
that translation unit plus the SDK, with no alternate native project, wrapper,
picture helper, callback, dormant native source or additional include surface.
There is **one**, not two, tracked command manifest:
`facturx/manifest.json`, declaring
`PDFA Embed FacturX(&T;&J;&T;&C;&J):J`, thread-safe, selector 1.
The exported dispatcher agrees; Windows `facturx.def` exports `PluginMain` and
`FourDPackex`. The tracked x86_64 bundle contains only Info.plist, not another
manifest or native binary. SDK and qpdf/zlib/libjpeg-turbo sources are excluded
from production edits.

The compiled SDK is the upstream `https://github.com/4d/4D-Plugin-SDK.git`
submodule pinned at `cd97d1876d4258b56d2516e8ad2816f4ee93e0b5`.
Its `4D Plugin API/4DPluginAPI.c` SHA-256 is
`24f37a868431ca1d6b0ba27b5b3cc029934417949d11d5c8aa5dbfc11afcf3d5`.
The source's v14 comment is not an upstream release identification.

Both getters declare uninitialized output and forward its address to
`EX_GET_OBJ_VALUE`; only success permits owning adoption and clearing.
Successful scalar reads copy into C++ values before the owning `PAVar` clears;
wrong types also clear. Text/collection setters shallow-alias locals, while
engine property/collection assignment copies/retains values, allowing the
local variable's one clear. `object_set_collection` consumes the creator's
reference; `make_status` returns its owning object, which `PA_ReturnObject`
hands to the runtime without an extra release. No `CreateVariable(Unistring)`
initial allocation is overwritten in production. Parameter strings/objects/
collections are borrowed and are not cleared.

SDK array setter acceptance is exactly Unicode-array type, non-null data,
0 <= index <= count, non-null string pointer. It disposes the prior value and
shallow-transfers the input; rejection retains caller ownership. Array getter
results are borrowed. `PA_SetStringVariable` aliases; `PA_ReturnString` instead
calls the engine content setter. No first-party JSON Parse, method/command
invocation, pointer or picture wrappers exist. SDK creator helpers internally
invoke commands; their successful returned refs are handed off, not cleared
twice. `PA_ExecuteCommandByID` handle failure's raw-kind-zero sentinel must not
be treated as a valid string or cleared; no first-party direct caller exists.

Deferred SDK-only issues include collection-length failed/uninitialized output,
resize-OOM count corruption, and creator allocation/command failures. Ambiguous
pointer metadata, app-path and picture-data ownership are not inferred here.
The shim does not certify real 4D engine behavior, Windows execution, or the
originating Poppler crash. Normal array tests do not certify OOM behavior.
No release, signing, packaging, tag, publication or manual dispatch is needed.
