# Legacy projection schemas

`udwm.json` and `dwmcore.json` are the only editable sources for OpenGlass's DWM Symbols, projected bindings, exact DbgHelp `UNDNAME_COMPLETE` names, version ranges, Layouts, ABI fallbacks, and reverse-engineering guidance. Do not edit the generated files under `$(IntDir)\Generated\Projection`, and do not duplicate this inventory in C++.

Use `notes` only for lasting, non-obvious guidance: semantic anchors, constructor and xref routes, adjusted-`this` calculations, ABI traps, ICF/inlining ambiguity, and independent cross-checks. Exact PDB names, ranges, visibility, ordinary consumers, and migration provenance belong in structured fields or source references, not in notes.

The optional top-level `min_inclusive` and `max_exclusive` values bound a module's whole inventory. Legacy declares `[17763.0, 28000.0)`, and `ModuleRegistry::Freeze` rejects any version outside that range before symbol collection or hook preparation. A final Layout `otherwise` case carries the last known offset through later servicing revisions within the range, but never across the build 28000 boundary into MILComp.

The required top-level `known_builds` list records the build families a module explicitly recognizes. Codegen emits it into `ModuleRegistry`, and startup skips the new-Windows-version warning only when both `uDWM.dll` and `dwmcore.dll` recognize their current builds. This warning is independent of the module range and Required projection validation; a known build is not by itself a claim of runtime support.

The generator uses only the Python standard library and runs as an incremental MSBuild step. To run it by hand from the repository root, writing into the default x64 Release intermediate directory:

```powershell
python Scripts/projection_codegen.py --repo . --architecture legacy --platform x64 --output Cache\OpenGlass.Legacy\x64\Release\Generated\Projection
```

Add `--check` to validate without writing files. The generator validates the schema first, then atomically replaces each changed output file. Offset expressions remain C++ source text that Python never evaluates. Layout notes become comments in the generated headers and add no runtime data. Symbol names are stored verbatim in a deduplicated NUL string pool, and naturally aligned runtime specs and state are indexed rather than self-registering. Stable IDs are diagnostic labels; `symbol_names` alone decides matching. Do not add name-only, substring, decorated-name, or first-match fallbacks.

## CPU platforms

One schema serves both CPU platforms. The optional `platforms` array on a Symbol or Layout accepts `x64` and `arm64`; the `AMD64` and `ARM64EC` spellings normalize onto them, and an entry without `platforms` applies to both. The project files pass `--platform $(Platform)` and build only `x64` and `ARM64`, so each CPU platform generates its own symbol and layout headers plus registry. The source check applies the same platform to the handlers: it keeps the `#if defined(_M_ARM64)` branch for ARM64 and the `#else` branch for x64, so a projected wrapper or a raw Symbol consumer must sit in the branch that belongs to its platform. A `platforms` filter that leaves a projected function or Symbol handle without a consumer is a codegen error, never a silent fallback.

A binding may also carry `arm64_symbol_names`, which replaces `symbol_names` when resolving ARM64 PDBs, and a Symbol may carry `arm64_type`, which replaces `type`. Both exist because DbgHelp spells the same logical function differently per platform, and because some aggregates are passed differently: x64 lowers a by-value `gsl::span<const BYTE>` parameter to a reference, while AAPCS64 passes the 16-byte aggregate in two registers, so the detour signature and the projected ABI differ per platform.

Use `platforms` only for facts that really differ. The verified ARM64 substitutions in this architecture are:

- `CWindowList::GetWindowListForDesktop` is not exported on 23H2. ARM64 keeps it as inlined copies there, so `GlassKernel.cpp` reimplements it from `CGenericTableMap<unsigned __int64, CWindowList::DESKTOP_WINDOWLIST_MAP_ENTRY>::FindElement` plus the `CWindowList.DesktopWindowListMap` and `CWindowList.DESKTOP_WINDOWLIST_MAP_ENTRY.RList` Layouts, and reports an unknown desktop instead of a sentinel list. Windows 11 24H2 exports the helper on ARM64 again and drops `FindElement`, so that descriptor is bounded to `max_exclusive 26100` and a separate ARM64 descriptor covers 26100 and later.
- `CDesktopManager::IsHighContrastMode` and `CTopLevelWindow::IsShadowNCAreaPart` are not called on 23H2: ARM64 reads the high-contrast flag inline and evaluates the shadow-part test inline inside `CTopLevelWindow::UpdateWindowVisuals`, so the x64 call-site scan is replaced by ARM64 instruction patches anchored on that function. Only the read that captures the bool consumed by the later background and border code is forced; forcing the rectangle-clear read removes the background fill, and faking the flag for the whole call also reaches the rounded-border color and paints a black ring. Forcing that read alone is not enough: the forced block also evaluates the inlined `(part - 0x12) <= 3` shadow-part test that x64 separately forces out of range, and leaving it alone sends the shadow NC area parts down their special path, which drops the drop shadow as soon as the modern border is disabled. Both 23H2 decisions are forced from the same anchored search window, and the branch is re-encoded as an unconditional branch to its own target. Both are verified on the 22631 ARM64 build.
- From 24H2 both predicates are real calls again, and the 26100 ARM64 binary has neither inline form: `UpdateWindowVisuals` contains a `bl CDesktopManager::IsHighContrastMode` and a `bl CTopLevelWindow::IsShadowNCAreaPart` instead. The ARM64 patch stage therefore replaces each call with `mov w0, #1` and `mov w0, #0`, which forces the same results without touching the consumer that follows. Both symbols resolve on 26100 ARM64, and the call sites are unique inside the anchored window.
- `CDrawingContext::FillShapeWithBrush` is not exported by 26100 ARM64: every caller inlines it there. `CDrawingContext::FillShapeWithSolidColor`, which drives the same device-context fill, still exists, so the ARM64 renderer switches to it from 26100 on and the brush descriptor is bounded to `max_exclusive 26100`.
- The draw-geometry command type is a build constant for ARM64 because the x64 instruction scan cannot match AArch64 code: 444 on 23H2 and 0x1A1 (417) on 24H2. Both values are read from the store that `CRenderDataBuilder::DrawGeometry` performs at command offset 4, which is the same slot the x64 scan reads.

Handlers must reach private DWM fields through typed Layout accessors. Add a focused Layout descriptor and accessor rather than embedding a byte offset or a padding-based fake object in a handler. Codegen validates the schema and its projected consumers but deliberately does not parse arbitrary handler C++, so reviewers must enforce this boundary. The rule does not apply to instruction-pattern navigation, COM vtable indexing, or application-owned structures.

Raw Symbols use their exact function-pointer type. The one exception is an instruction-pattern anchor, which must be declared as `BYTE*` with `usage: "code_address"`. Model projected ABI changes with disjoint version ranges. If the OpenGlass wrapper intentionally discards an old return value or supplies one extra trailing Win64 argument, declare the exact source `type` plus `abi_compatibility`; the generated code asserts the relationship at compile time. Do not use this mechanism to cover reordered, removed, or otherwise incompatible arguments.

Each logical Symbol owns one typed slot and a non-empty, ordered `bindings` array. A binding holds the exact `symbol_names` and one `[min_inclusive, max_exclusive)` resolution interval. Bindings within a Symbol must not overlap, and a version that no binding covers makes the Symbol version-inactive rather than unresolved. Merge versioned descriptors into one Symbol only when module, semantic target, kind/usage, requirement, fallback, and underlying ABI are all identical. If the ABI changes, keep separate logical Symbols even when their intervals don't overlap.

Once startup has selected each Layout case, normal `read/ref/address` access is a direct offset load plus address arithmetic. Descriptor lookup, version selection, validation, allocation, locking, and exceptions stay out of the field-access hot path. The checked `offset()` API remains available for startup diagnostics.

Run the Layout and Symbol schema validators before and after edits. The Symbol validator requires every projected function to have a real wrapper call site or a direct typed Symbol consumer; declaring the wrapper is not enough. A normal Release build lets LTCG inline projected wrappers. When you change the projection mechanism, binary size and generated machine code are worth inspecting, but they are development diagnostics, not fixed acceptance thresholds:

```powershell
python .agents/skills/maintain-dwm-offsets/scripts/validate_projection_layouts.py . --architecture legacy --version 26100.8972
python .agents/skills/maintain-dwm-offsets/scripts/validate_projection_symbols.py . --architecture legacy
msbuild OpenGlass\OpenGlass.Legacy.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Schema changes alone establish neither semantic correctness nor OS support. Treat an exact-binary semantic audit and real-OS validation as separate release gates.
