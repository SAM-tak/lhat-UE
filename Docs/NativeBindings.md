# Generated native bindings

The native binding pipeline uses UnrealHeaderTool (UHT) for discovery and a
Python generator for direct C++ calls. There is no `LhatExpose` metadata and no
per-class exposure allowlist. Its input is the project's **Editor target** plus
the editor's actual enabled-plugin inventory, including editor/plugin APIs.
Blueprint-authored classes are not inputs to this generator.

The intended deployment is now three-tier: cached reflection for general
native/Blueprint APIs, prebuilt plugin-owned direct bindings for hot Engine
APIs, and optional project-generated C++ for project-specific acceleration.
The current generator is the third tier's foundation, not a requirement that
every plugin user create a C++ project. The plugin now carries a
[bundled Engine acceleration set](BundledEngineApi.md) and a binary packaging
workflow. [Automatic dynamic bindings](DynamicBindings.md) now fill supported
names absent from both static providers. See
[BindingBenchmark.md](BindingBenchmark.md) for the opt-in comparison harness.

## Generate and build

Requirements: the plugin's LhatCore libraries and Lhat/LhatEditor modules have been built, UE 5.8.2's
bundled .NET SDK is available, and Python 3.11 or later is on PATH. Close the
editor and wait for other builds/generation processes to finish first.
When upgrading from the initial three-type binding, run `Scripts/BuildLhat.ps1`
again before rebuilding UE: this also creates the new cross-DLL C API headers.

From the plugin directory:

```powershell
.\Scripts\GenerateNativeBindings.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject -Install -Build
```

The command refreshes UHT's header inventory without compiling old wrappers,
exports a flat reflection snapshot into a new directory under
`<Project>/Saved/LhatNativeBindings/`, captures enabled plugins with the
`LhatNativeContext` commandlet, generates C++, then builds it. Use
`-Target CustomEditor` if the target name differs from the project filename.
`-Python` selects the Python executable. Omit `-Build` to inspect output first.

`-Install` adds two project modules to `.uproject` without changing the enabled
plugin list. It is idempotent. Subsequent generation can omit `-Install`.
The generated modules are Win64-only, matching the plugin's current platform.
Project generation subtracts the bundled manifest's functions, checking the
signatures and Runtime/Editor placement. Shared native types use one tag across
providers. After upgrading from a pre-bundle project generation, regenerate
and rebuild these optional modules before launching: old callbacks would
otherwise attempt to register bundled members twice. The existing project
source files remain owned/protected by the generator's hash manifest.

For a previously exported snapshot:

```powershell
.\Scripts\GenerateNativeBindings.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject `
    -SkipExport -SnapshotDirectory C:\Path\To\Snapshot -Build
```

The snapshot directory contains `UHT/Lhat/<Module>.json`. The exporter is
shared with the API-size survey, but older measurement snapshots without
header information must be exported again. No engine source or LhatCore
source is modified. Generated C++ does not become part of LhatCore.
The enabled-plugin inventory is refreshed even with `-SkipExport` and stored
as `context.json` beside `UHT`. The Python CLI requires this context and checks
that it belongs to the same project. A build dependency is **not** proof that
a plugin is enabled: UHT can include headers from disabled plugins.

After rebuilding, run the existing `Lhat.DumpHostApi` console command or
`-run=LhatDumpHostApi` commandlet to refresh `<Project>/lhat-host.json`.
That JSON comes from the actual compiled registrations, not a prediction of
what the runtime might support.

## Output and registration

| Output | Responsibility |
| --- | --- |
| `Source/LhatGeneratedRuntime/` | Runtime native wrappers and module dependencies |
| `Source/LhatGeneratedEditor/` | Editor/developer/uncooked-only wrappers; excluded from game targets |
| `Source/LhatGeneratedRuntime/LhatGeneratedFiles.json` | Generated-file ownership hashes; keep with generated sources |
| `Saved/LhatNativeBindings/report.json` | Generated signatures, skipped functions/reasons, counts, dependencies, input hashes |

The generated project modules can depend on game/plugin modules without
making the Lhat runtime depend on the game or the editor. They register
providers at module startup. Their loading phase is `None`; `FLhatProgram`
loads the configured generated modules explicitly before gathering providers,
registers object types in base-before-derived order, then registers functions.
This lets the inventory commandlet start without loading stale generated DLLs
when plugins or APIs change. A configured but unloadable generated module
causes a program initialization error, not a silent fallback to the small API.
Existing programs do not change when another provider loads. Generated modules disable dynamic
reload because callbacks retain addresses in those DLLs. Restart after changes.

Generated source is deterministic, split into bounded translation units, and
unchanged files are not rewritten. Edited/unowned files are not overwritten.
Stale files are removed only when their hashes match the ownership manifest;
unrelated files are never removed. Writes use temporary files and a pending
manifest to recover interrupted generation. `Saved` may be cleared without
losing the ownership record. Do not edit generated C++; edit the generator or
native declarations and regenerate instead.

## Names and calling convention

Classes use `ue.<NativeModule>.<UHTClassName>`, without the C++ `U`/`A` prefix.
`ue.Object`, `ue.Actor`, and `ue.Vector` retain their existing names. Accessible
base classes are registered even if they have no Blueprint methods; an
inaccessible base is represented by the nearest accessible registered ancestor.
Methods use their **UFUNCTION names**, not display names or `ScriptName` aliases.
The existing short Actor methods remain available alongside generated `K2_`
methods. C++ defaults are not yet mapped to optional Lhat parameters: pass all
declared inputs, including explicit world-context arguments.

```text
import^ue
let^ value = ue.Engine.KismetMathLibrary.Abs(-42.0)
```

`import^ue` includes the registered child modules, so it is sufficient when
mixing the short compatibility API with generated APIs. Do not redundantly add
`import^ue.Engine` to the same scope: the pinned core checks that combination
but faults at runtime when importing into a sealed parent table. Importing only
a child, or using separate expression aliases (`let^ Engine = import^ue.Engine`),
also works. See [the reproduction notes](KnownIssues.md).

An instance callback validates its receiver, converts arguments into native
locals, calls `Self->Function(...)`, and converts its return. A static callback
calls `UClassName::Function(...)`. The callbacks have the Lhat C ABI but are
compiled as **C++**, which is necessary for calling Unreal C++ APIs. There is
no `ProcessEvent`, UFunction lookup, or reflected parameter buffer on this path.

The C API itself is imported from the Lhat DLL. `BuildLhat.ps1` preprocesses
the pinned core's public headers, checks the actual static-library symbols,
and generates header copies decorated with UE's `LHAT_API`, plus an export
list. This is necessary because allocator/tag state must not be duplicated in
the Runtime, Editor and generated DLLs. Inline value helpers remain local;
native UE calls remain direct. No language-core source changes are needed.
The pinned core's C-side `lhat_machine_call_member` erases a wide hostvalue
return to `nil`; test/use vector-returning APIs inside Lhat code, where the VM
has correctly sized result slots. This does not affect ordinary Lhat calls.

UObject handles use weak references, preserve identity and the most-derived
registered native type, and reject destroyed objects or incompatible receivers.
They do not keep objects alive. UObject parameter/return types include `nil^`;
receivers cannot be nil. As with Blueprint object pins, pointee `const` does not
create a separate immutable object type. All calls must run on the game thread.
These are native APIs with native side effects, not a security sandbox.

## Initial supported surface

- Ordinary exported public native UFUNCTIONs marked BlueprintCallable/Pure,
  including instance and static methods, and methods in editor modules.
- bool; signed/unsigned 8/16/32/64-bit integers; float/double; FString/FName;
  exported UObject-derived pointers.
- FVector, FVector2D, FRotator, FQuat, FTransform, FLinearColor and FColor as
  copied native host values. Plain numeric fields are registered except for
  FTransform, which should be manipulated through generated math functions.
- UE BlueprintPure maps to `f^`; other calls map to `p^`. Lhat has one
  `number^` type; integer inputs are checked for integrality/range at the
  boundary. int64 remains exact. uint64 values beyond INT64_MAX are rejected.

Unsupported static functions are **reported** by this generator. A separate
[runtime reflection pass](DynamicBindings.md) can now bind some of them,
including FText, class references, out/ref parameters and Blueprint bodies.
The static generator does not cover enums, FText,
arbitrary structs, arrays/maps/sets, delegates, interfaces, class/soft/weak
pointer wrappers, mutable reference/out parameters, events/RPCs, latent calls,
custom/wildcard thunks, or dynamically determined return types. UPROPERTY
getters/setters and automatic conversion of C++ default arguments are later
extensions of this generator. Blueprint-defined classes use the dynamic path.

Blueprint exposure alone does not guarantee an externally callable C++ API.
Private/protected methods, missing DLL exports, private/internal headers, and
public headers that require private/internal headers cannot be called by an
external generated module. In particular, `MinimalAPI` exports a class's type
identity, **not all of its methods**. These cases have explicit report reasons.
Ordinary public-header module dependencies are resolved automatically; editor
include dependencies on runtime wrappers are conditional on `Target.bBuildEditor`.
No engine-private include paths are added as a workaround.
Disabled plugins and explicitly loaded/game-feature plugins are excluded;
generation never enables or mounts them. Dynamic plugin activation will need
per-plugin providers in a later extension.
UBT can warn that the project does not explicitly list a plugin used by a
generated module even when other enabled plugins bring it in transitively.
The generator leaves the project's plugin list unchanged; it does not suppress
these warnings or pin every currently enabled plugin into that list.

After updating this plugin itself, rebuild its two modules before generation.
If old generated C++ cannot compile, the scoped bootstrap command is:

```powershell
& C:\Path\To\UnrealEngine\Engine\Build\BatchFiles\Build.bat ProjectEditor Win64 Development `
    -Project=C:\Path\To\Project\Project.uproject -Module=Lhat -Module=LhatEditor -WaitMutex -NoHotReload
```

This is a development-editor binding pipeline, not yet proof of a packaged
Shipping build. The target's enabled plugins/configuration are inputs, not an
immutable global API catalog. Regenerate after changing UE, native headers,
target settings or plugins. Raw-source staging and VM-only Shipping remain
separate tasks.

## Tests and extension points

Validated on FirstPersonTemplate, UE 5.8.2 / Win64 Development Editor,
2026-09-13: 2,273 Runtime classes / 5,246 functions and 400 Editor classes /
1,261 functions are generated. The actual host export, including compatibility
bindings and math types, contains 2,682 types and 6,516 functions in
1,468,419 UTF-8 bytes. This supported-signature subset is not the full API-size
survey and includes neither generic property accessors nor full documentation.
All 12 UE automation tests and 21 Python tests pass. Fresh UHT export through
the CLI also completes; repeating generation leaves C++ unchanged (zero build
actions). Packaged/Shipping targets have not been validated.

```powershell
python .\Scripts\TestNativeGenerator.py
python .\Scripts\TestCoreApi.py
.\Scripts\TestLhat.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject
```

The generator tests check direct-call emission, editor separation, access/out
rejections, deterministic output and generated-file ownership. With generated
modules installed, `Lhat.Editor.GeneratedNativeCalls` tests actual compiled
callbacks (including a ProcessEvent counter), UTF-8, nullable object identity,
GC, math values and numeric range checks. `GeneratedNativeScript` checks that
Lhat source can compose a runtime API and an editor API. Without generated
modules these two integration tests explicitly report that they were skipped.

Extend UHT's flat IR in `Scripts/UhtSurvey/SurveyExporter.cs`, signature/argument
selection in `GenerateNativeBindings.py`, and conversion primitives in
`Source/Lhat/Public/LhatNativeBindings.h`. Add tests before admitting another
codec. Enum/struct support and multi-return handling should extend this pipeline,
not introduce per-UCLASS Lhat exposure annotations.
