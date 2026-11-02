# Projection source organization

`legacy` and `milcomp` identify DWM backends, independently of the CPU target.
Generation selects one CPU with `--platform` (default x64) from these same
backend directories. This fork builds and ships the DWM DLLs for both x64 and
ARM64, so both CPUs' groups are live inventories rather than tool-only targets.

Each architecture owns independent schema-version-5 module manifests, `udwm.json` and `dwmcore.json`. A manifest contains module metadata (namespace, tag, recognized builds, supported version range, and module notes) and explicit `symbols` and `layouts` file lists. Paths are relative to the manifest. There is no recursive inclusion, directory discovery, or inline descriptor format.

Each descriptor occurs once. Layout `cases` and Symbol `bindings` contain platform
groups with a `ranges` array. `platform: null` or an omitted `platform` denotes the
common group. The selected CPU's group replaces the entire common group, regardless
of group order. If neither exists, the descriptor is omitted from that CPU's inventory.
Only the selected group's ranges and ABI fields are validated. Version gaps within
a specialization do not fall back to common ranges.

```json
{
  "name": "Widget_Field", "id": "Widget.Field", "kind": "field", "type": "int",
  "cases": [
    {"platform": null, "ranges": [{"offset": 16, "until": {"build": 26100}}, {"offset": 24, "otherwise": true}]},
    {"platform": "x86", "ranges": [{"offset": 8, "otherwise": true}]}
  ]
}
```

Symbol groups own `target`, `type`, `fallback` and `abi_compatibility` when applicable, and their ranges own exact `symbol_names` and version intervals. Symbol ranges are ordered by `min_inclusive` and use `[min_inclusive, max_exclusive)` intervals. Layouts keep their
shared `type` on the descriptor, but a group may override it. Names, IDs, kinds and other
descriptor metadata are not merged between entries. Duplicate handles or duplicate
common/selected CPU groups are errors. Each selected group needs non-empty ranges.

The loader normalizes the selected group before assigning indices/slots or checking
source consumers. Diagnostic locations refer to the descriptor's `name` value.
Empty inventories produce empty tables. Catalog selection is described in
[CPU target selection](../../tools/README.md#cpu-target-selection).

For a CPU-only Symbol or Layout, omit the common group. Guard its C++ handle uses,
facade members/wrappers and callers with matching compiler CPU conditions. An
omitted descriptor creates no slot or Required prerequisite. Source inspection
checks selected consumers and wrappers, while the C++ compiler owns unresolved raw
Symbol and Layout references. Compiler CPU macro profiles can overlap, so predicates must match the intended group rather than assuming every macro is exclusive.

### ARM64 substitutions

The ARM64 inventories carry platform groups only where the verified ARM64 binaries
differ from x64. The confirmed differences are:

- `CWindowList::GetWindowListForDesktop` is not exported and survives only as inlined
  copies, so `GlassInfra.cpp` reimplements it from
  `CGenericTableMap<unsigned __int64, CWindowList::DESKTOP_WINDOWLIST_MAP_ENTRY>::FindElement`
  plus the `CWindowList.DesktopWindowListMap` and
  `CWindowList.DESKTOP_WINDOWLIST_MAP_ENTRY.RList` Layouts, and reports an unknown
  desktop instead of returning a sentinel list. Both Layouts are arm64-only.
- `CDesktopManager::IsHighContrastMode`, `CTopLevelWindow::IsShadowNCAreaPart` and
  `CRenderDataBuilder::DrawGeometry` are neither exported nor called on ARM64. They
  stay x64-only descriptors: the x64 call-site scan and instruction scan are replaced
  by the ARM64 alternatives below, so an ARM64 inventory must not carry them as
  Required prerequisites.
- ARM64 reads the high-contrast flag inline inside `CTopLevelWindow::UpdateWindowVisuals`,
  so the x64 call-site scan is replaced by a byte-pattern patch anchored on that
  function (`CTopLevelWindow::UpdateWindowVisuals.forcedHighContrast`, arm64-only,
  optional). Only the read that captures the bool consumed by the later background and
  border code is forced; forcing the rectangle-clear read removes the background fill,
  and faking the flag for the whole call also reaches the rounded-border color and
  paints a black ring. That one decision is not enough on its own: the forced block
  also evaluates the inlined `(part - 0x12) <= 3` shadow-part test that x64 separately
  forces out of range, and leaving it alone sends the shadow NC area parts down their
  special path, which drops the drop shadow as soon as the modern border is disabled.
  Both decisions are forced from the same anchored search window, and the branch is
  re-encoded as an unconditional branch to its own target. Both are verified on the
  22631 ARM64 build.
- The 23H2 draw-geometry command type is the constant 444. The x64 instruction scan
  cannot match AArch64 code, so `GlassInfra.cpp` selects it from the build instead of
  scanning.
- `CRenderData::TryDrawCommandAsDrawList` takes its `gsl::span<const BYTE>` by value in
  registers on AAPCS64, while x64 lowers the same 16-byte aggregate to a reference.
  Both versioned logical Symbols therefore carry separate x64 and arm64 binding groups
  with their own `type`.

Older ARM64 builds stay unsupported where their evidence is missing: the desktop-map
Layouts are verified only from Windows 11 on ARM64, and the ARM64 build reports an
unknown desktop below that boundary instead of guessing offsets.

The 24H2 occlusion, overlay and dirty-region patch targets, and the two 24H2 render
hook targets, are x64-only inventories. Their evidence is x64 machine code: byte
patterns, inlined comparison encodings and detour contracts that were audited on the
x64 binaries. ARM64 has neither those patterns nor an audited counterpart, so those
descriptors carry a `platform: "x64"` group and their consumers in
`GlassIntegrity.cpp` and `GlassReflectionHandler.cpp` sit inside
`#if !defined(_M_ARM64)`. The x64 inventory keeps every descriptor unchanged.

```text
<architecture>/
  udwm.json
  udwm/
    symbols/
      visuals.json
      window-frame.json
      ...
    layouts/
      CVisual.json
      CTopLevelWindow.json
      CWindowData.json
      ...
  dwmcore.json
  dwmcore/
    symbols/
      drawing-context.json
      occlusion.json
      ...
    layouts/
      CDrawingContext.json
      COcclusionContext.json
      ...
```

Symbol sections contain only a `symbols` array and group entries by resource or usage category, such as resources, geometry, drawing contexts, or occlusion. Layout sections contain only a `layouts` array and primarily group members by their owning class, with files such as `CVisual.json` and `CTopLevelWindow.json`. These partition strategies are independent: a Symbol category may span several classes, and a class's Layouts need no matching Symbol file. Keep each descriptor, including all of its version cases or bindings and evidence notes, together. Keep every platform group of a descriptor together. Do not split by Windows version or duplicate its handle.

Every Layout case's `offset` is a signed 32-bit JSON integer measured in bytes, including vtable byte offsets and negative adjusted-`this` displacements. Use decimal numbers, never strings, `sizeof`, `offsetof`, or C++ arithmetic. The values describe the selected module's binary layout independently of compiler-side types. Validation enforces the integer representation and range once, and codegen emits the numbers directly without expression evaluation or redundant generated range assertions.

Handler code accesses private DWM fields through typed Layout accessors rather than object arithmetic or padding-based fake layouts. This scope excludes instruction-pattern navigation, COM vtable slots, and ordinary application-owned storage. Codegen does not parse arbitrary C++, so review actual consumers instead of relying on cast-spelling heuristics.

Typed Layout member access covers private object fields, array records, and pointers into opaque native storage. Native record storage preserves verified size and stride separately from member offsets without padding-based member layouts. Non-owning views of native records do not take over native construction, copying, or destruction. Neither a nearby member nor an allocation size alone provides field evidence.

Layout `until` boundaries use the same numeric version objects as Symbol bindings. In each boundary object, `build` is a nonzero unsigned 32-bit JSON integer, and `revision` is an unsigned 32-bit JSON integer that defaults to zero when omitted. For example, `"until": {"build": 26100, "revision": 4484}` is an exclusive right boundary. Numeric strings and C++ constant names such as `os::build_w11_24h2` are not supported, and schema loading does not parse `OSHelper.hpp`. Layouts and Symbols share version validation and one deduplicated numeric version pool in generated metadata.

The `otherwise: true` case is the explicit open-ended Layout case and must be last. Without it, later versions resolve to `unsupported`. An open end is not evidence of correctness on unanalyzed future builds.

The loader combines the listed sections and validates each module as one inventory. Handle names remain unique across sections. Stable-ID and target ranges are checked across the whole module, preserving disjoint Symbol variants that share a diagnostic ID. All existing ABI, interval, fallback, and consumer rules still apply. Descriptor order is deterministic by stable ID, binding range, and handle name, independent of section order. Source partitioning adds no runtime metadata, registry, locking, or dispatch layer. Layout and Symbol audit reports identify the editable section and declaration line.

Descriptor integrity is established before runtime. Schema validation owns non-overlapping binding intervals, ordered Layout cases and consistent fallbacks, and codegen constructs table indices and slices from the validated inventory. `ModuleRegistry` consumes these internal generated tables directly. `Freeze` checks the actual module version, selects Layout cases and resets resolution. It does not revalidate generated indices or rescan binding intervals for overlaps.

The Python tools share `projection_schema.Version`, range checks and binding selection. Schema loading validates version representations once. Emitters, catalog matching and query reports select from those validated ranges directly. Reporting does not repeat Layout uniqueness checks or impose uniqueness across independent modules. CLI argument parsing owns the architecture choice, and internal emitters consume the loader's fixed module inventory. [Validation ownership](../../tools/README.md#validation-ownership) describes the full tool pipeline.

Codegen groups projected Symbols by kind and normalized target, emitting one `BindingSpec` per physical slot with its fallback and a span of candidate logical Symbol indices. ABI variants share a group, and multiple bindings of one logical Symbol contribute only one candidate index. `PublishBindings` visits these groups and their candidates once, publishing the first committed resolved address or the group's fallback. Reset restores each slot's fallback. There is no runtime slot-identity search or grouping pass. Required/Optional status, Debug-only activation, version gaps, exact-name ambiguity and cross-module Validate/Commit still depend on actual resolution and retain their runtime behavior.

The built-in symbol catalog supplies a prevalidated dense RVA span for the loaded module's PDB identity. Its collection path stages candidates directly, without descriptor searches or a separate Required validation pass. Catalog collection leaves published slots untouched, while PDB parsing begins with an explicit Reset to fallbacks and then collects candidates. Neither path publishes collected addresses until the common two-module commit succeeds. Missing-symbol reports are constructed only after that gate fails. See [the catalog contract](../SymbolCatalogs/README.md) for the generation and binary-evidence boundaries.

A successful commit replaces the complete resolved set, including clearing Optional Symbols that are now missing or ambiguous. Collection alone keeps the previous published set, and a failed two-module validation publishes neither module's staged candidates. Reset remains the explicit operation that clears resolution and restores slot fallbacks before a new initialization or PDB parse.

When adding or moving a section, update its manifest and the architecture project's explicit `None` items and `.vcxproj.filters` entries. The generation target uses the project's JSON `None` items as inputs, covering both projection sections and symbol catalog shards. Keep each file listed separately so Visual Studio can edit the project. Do not use item-list expansion or source wildcards in project items. Generated C++ stays under the selected project's `$(IntDir)\Generated\Projection` and is never an editable inventory. Compatibility `*.Offsets.hpp` headers expose generated Layout declarations rather than another offset inventory, and they can also contain forward declarations or type aliases.

## Runtime access and ABI

`SymbolHandle` and `FieldHandle` encode module/index/type. Each logical Symbol owns one resolution state and typed slot, even with several bindings. Required Symbols are global startup prerequisites only when their current-version binding is active, and inactive version gaps do not participate. Optional projected functions require an explicit ABI-compatible fallback. A version-inactive projected function uses its declared fallback, or the typed FailFast thunk if none was declared. Keep the common two-module validation gate without component feature masks or partial readiness.

`FieldHandle::address()` and `ref()` preserve the base object's constness. Use `mutable_address()` or `mutable_ref()` only inside a projected class facade whose established API intentionally exposes a mutable private subobject from a const method. Never use them directly from handler code to bypass constness.

Raw Symbols must declare their real function-pointer ABI. Use `BYTE*` with `usage: "code_address"` only for instruction-pattern navigation that intentionally treats a symbol as bytes. Merge versioned entries into one logical Symbol only when module, semantic target, kind/usage, requirement, fallback, and underlying ABI are identical, and express their exact names and disjoint intervals as separate bindings. If an ABI changes across builds, keep separate typed logical Symbols even when their ranges are mutually exclusive. The exceptional `abi_compatibility` forms (`discard_return` and `extra_trailing_argument`) require an explicit source type and are compile-time checked. Never hide an ABI change by placing incompatible complete names in one logical Symbol.

An exact function-pointer ABI does not by itself prove that an internal DWM function is safe to detour through an ordinary C++ dispatcher. Whole-program optimization may let a caller rely on a known callee, or a short wrapper around it, preserving volatile GPRs, XMM lanes, or EFLAGS beyond the public Windows x64 ABI. Audit every hooked function on the exact binary by following all direct callers and intervening wrappers, and check values live across the call before approving a normal `Detour` or `ChainDetour`. Treat register allocation and instruction patterns as candidate evidence only. Prove the downstream semantic use and eliminate infeasible paths and value-independent idioms. When a private contract is verified, preserve it in the physical dispatcher with the narrowest version-specific `CustomDispatchDetour`/assembly shim, document the decisive call chain, and keep replacement dispatch plus `HookRundown` inside the preserved region. Do not generalize one confirmed contract to other functions or builds, and do not misrepresent it as a projected signature change.

After `Freeze`, `FieldHandle::read/ref/address` must directly load the selected module offset and perform address arithmetic. They must not call the registry, scan version cases, validate descriptors, allocate, lock, or throw. The explicit `offset()` API remains a checked cold-path diagnostic. Metadata clarity takes priority over byte packing because descriptor traversal is startup-only.

Every projected wrapper must be `inline` and contain only an `OPENGLASS_MUSTTAIL return Projection::Invoke<&Target>(...)` dispatch. `OPENGLASS_MUSTTAIL` expands to `[[msvc::musttail]]` outside Debug, while `/Od` Debug uses a normal debuggable forwarder. Normal Release/LTCG may inline the wrapper into a direct call through the typed slot, but `inline` is not treated as a guarantee. Do not use `__forceinline` because MSVC may still refuse it and emit C4714 warnings. Do not add range checks, `.get()`, fallback logic, runtime instruction patching, or `VirtualProtect` to projected wrappers.

A wrapper declaration alone is not a consumer. Every projected function must have a real runtime call site or a direct typed Symbol consumer such as a Detour. Remove unused wrappers and descriptors rather than making them needless Required startup gates. Raw symbol access, Detour preparation, and pattern anchors use the checked cold path.

## Backend selection

DWM backend is independent of CPU platform (`--platform`, defaulting to x64). Never share Layouts, Symbol bindings, complete names, or ABI between architectures solely on stable-ID similarity.

- **Legacy**: schemas in `legacy/`, consumers in `OpenGlass/Architecture/Legacy/`, project `OpenGlass.Legacy.vcxproj`. Module interval is `[17763.0, 28000.0)`.
- **MILComp**: schemas in `milcomp/`, consumers in `OpenGlass/Architecture/MILComp/`, project `OpenGlass.MILComp.vcxproj`. Module interval is `[28000.0, unbounded)`. An open upper bound does not imply verified support for unaudited builds.

Top-level `known_builds` lists recognized build families. Startup suppresses the new-Windows warning only when both `udwm` and `dwmcore` recognize the active build. This check is independent of module-range and Required-symbol validation.

Use `notes` only for non-obvious reverse-engineering guidance: semantic anchors, adjusted-`this` displacements, ABI traps, ICF/inlining ambiguity, and independent cross-checks. Notes should name concrete native producer, consumer, or constructor routes rather than anonymous decompiler variables or assumed cross-build stability. Do not restate names, ranges, or consumer lists, or replace useful guidance with generic migration or PDB provenance. See [runtime access and ABI](#runtime-access-and-abi) for field/wrapper contracts and [tools/README.md](../../tools/README.md) for validation ownership and audit commands.

Code generation runs incrementally in MSBuild. Pass `--check` to validate without writing files. To run generation and validation manually:

```powershell
python -m tools.projection_codegen --repo . --architecture legacy --output Cache\OpenGlass.Legacy\x64\Release\Generated\Projection
python -m tools.projection_codegen --repo . --architecture milcomp --output Cache\OpenGlass.MILComp\x64\Release\Generated\Projection
python -m tools.validate_projection_layouts . --architecture legacy
python -m tools.validate_projection_layouts . --architecture milcomp
python -m tools.validate_projection_symbols . --architecture legacy
python -m tools.validate_projection_symbols . --architecture milcomp
```
