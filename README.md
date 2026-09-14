# lhat-UE

Runtime binding for embedding [L^](https://github.com/SAM-tak/lhat) in Unreal
Engine projects.

## Current development target

- Unreal Engine 5.8.2
- Windows 64-bit
- Visual Studio 2022 or 2026 with the C++ game-development workload
- CMake 3.25 or later
- Python 3.11 or later (binding/API header generation)

The plugin owns its L^ dependency as the `LhatCore` git submodule. It
does not refer to a project-relative checkout of the language core. The pinned
submodule revision is therefore part of each `lhat-UE` commit and is reproduced
by a normal clone.

## Initial checkout and build

Clone this repository with its submodules, or initialise the dependency in an
existing clone:

```powershell
git submodule update --init --recursive
```

Build the C libraries before generating or compiling an Unreal project:

```powershell
.\Scripts\BuildLhat.ps1 -Configuration Development
```

The script configures L^ with its compiler front end enabled and its command
line, language-server, debugger, standard-library and test targets disabled.
It produces the static `lhat` and `lhatport` libraries under
`Intermediate/LhatCore/Win64`. Those files, including the generated
`lhat/version.h`, are deliberately not committed.
It also generates UE-decorated public C headers and a DLL export list. All UE
modules use the **same LhatCore instance inside the Lhat DLL**, rather than
separate static-library copies with incompatible allocator state. The original
LhatCore headers remain unchanged. Use `-Python` to select a Python executable.

For a debug build, use `-Configuration Debug`. All other Unreal configurations
link the Release variant.

## Using it in an Unreal project

For a matching-engine **binary package**, copy its `Lhat/` directory into
`<Project>/Plugins/Lhat/` and enable the plugin. Blueprint-only projects do not
need `Source/`, generated project modules, CMake, Python or a LhatCore checkout
to use the bundled Engine API in the Editor. See
[BundledEngineApi.md](Docs/BundledEngineApi.md) for the included API, package
build command and binary-only smoke test. Binary packages are specific to the
UE build/platform/configuration used to produce them.

For a **source checkout / plugin development**:

Place this repository under a project's `Plugins/lhat-UE` directory (as this
development project does) and enable the **Lhat** plugin in the `.uproject`
file. After the bootstrap command above, generate project files and build the
editor target normally.

## Try the component

1. Copy `Examples/Script/Mover.lh` to `<Project>/Script/Mover.lh`. This is a
   plain UTF-8 source file, outside `Content`; it is not a `UAsset`.
2. Add a **Lhat Component** to a placed Actor or Blueprint. To see movement,
   use an Actor with a **Movable** root component (for example a movable cube).
3. Set **Script Path** to `Mover.lh`, relative to `<Project>/Script/`.
   **Parameters** exposes the annotated `Speed` field, in UE units per second.
4. Start PIE. The Actor moves along world X; `LogLhat` in the Output Log
   reports attachment and shutdown. Different component instances can use
   different speeds.
5. Stop PIE, edit the `.lh` file, and start PIE again to load the new source.
   Click **Refresh Parameters** while stopped after changing exported fields.

Callbacks use UE names: `BeginPlay()`, `Tick(deltaSeconds)`, `EndPlay()`.
They are optional and run on the game thread. `EndPlay` currently takes no
reason argument. An entry module must publish exactly one `def^`, whose
`new(actor: ue.Actor)` returns its instance; see the complete Mover example.
There is one script per component; multiple components may attach to an Actor.

Use `@EditAnywhere` on `self^` fields with number, bool or string defaults.
L^ annotations have a flat namespace, so this is **not** `@ue.EditAnywhere`.
The component serializes these fields in an `FInstancedPropertyBag`:

- Only values differing from the last inspected defaults override the script.
  Untouched values follow new source defaults on the next play session.
- Refresh preserves overrides with the same name and exact type. Removed or
  retyped fields are dropped/reset. Renames are treated as remove + add.
- Parameter names must also be unique ignoring case (UE uses `FName`).
- Refresh evaluates module-level declarations in a temporary VM, without an
  Actor and without calling `new` or lifecycle callbacks. Keep top-level code
  declarative; it is not an editor-time Actor execution hook.

## Initial binding surface

The `ue` module currently exposes these explicit bindings:

| Type | Members |
| --- | --- |
| `ue.Object` | `GetName()`, `IsValid()` |
| `ue.Actor` (inherits Object) | `GetActorLocation()`, `SetActorLocation(vector)`, `SetActorHiddenInGame(bool)`, `IsActorTickEnabled()` |
| `ue.Vector` | writable `X`, `Y`, `Z` |
| `ue` | `Log(string)`, `MakeVector(x, y, z)` |

`ue.Vector` uses the actual UE `FVector` layout: three doubles in this target.
Construct it with `ue.MakeVector(1, 2, 3)`. Type-level `ue.Vector.new` is not
exposed due to an argument-layout issue in the pinned core; see
[known issues and reproduction notes](Docs/KnownIssues.md).
`SetActorLocation` returns bool and currently performs a no-sweep move; it is
a small manual adapter, not the full UE overload with `FHitResult` output.
These initial methods now call C++ directly. Additional native Blueprint APIs,
including editor APIs, can be generated without Lhat-specific exposure metadata:

```powershell
.\Scripts\GenerateNativeBindings.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject -Install -Build
```

This creates project-side Runtime/Editor modules, direct C++ callbacks and an
explicit unsupported-API report. See [generated native bindings](Docs/NativeBindings.md)
for naming, supported codecs, regeneration and limitations. It is an initial
generation pipeline, not complete coverage of every Blueprint signature.

## Runtime ownership and diagnostics

`ULhatSubsystem` owns the compiled-program cache for each Game/PIE World.
Each entry's program is shared in that World, while `FLhatInstance` owns one
VM per component, roots its script state in L^, and keeps the program alive
until the VM is disposed. A new World starts with an empty cache. No hot reload
or state migration occurs during PIE.

UObject handles contain `TWeakObjectPtr` and **do not keep UE objects alive**.
Destroyed/disposed handles reject native access with a script error. UE and L^
GC lifetimes are independent; the plugin does not transfer UObject ownership.
Errors disable the failing instance's tick and appear in **Last Error** and
`LogLhat`, including source diagnostics/tracebacks. Each script invocation has
a budget of 100,000 backward branches to stop runaway L^ loops; this is not a
wall-clock timeout or a security sandbox for untrusted scripts/native calls.
Normal shutdown calls EndPlay once. Faulted instances and final GC cleanup
are disposed without re-entering script callbacks.

`FLhatModule` installs UE's `FMemory` allocator. `FLhatProgram` confines file
loading to its script root and exposes the native `lhat.h` API for additional
registrations before `Check()`. Use the owning `FLhatInstance` for runtime
lifetime management. Restart the editor after changing the C++ plugin; dynamic
module reload is disabled because native callbacks and type tags retain code
addresses.

## Generate lhat-host.json for the language server

Build the project's Development Editor target, then use either entry point.
Both export the same registrations as `FLhatProgram`, including UE types,
inheritance, function signatures, vector fields and `@EditAnywhere`. No map,
PIE session or valid `.lh` source is required.

In the Editor's Output Log console:

```text
Lhat.DumpHostApi
Lhat.DumpHostApi "Saved/Host API/lhat-host.json"
```

From a terminal (PowerShell example):

```powershell
& 'C:\Path\To\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
    'C:\Path\To\Project\Project.uproject' `
    -run=LhatDumpHostApi -unattended -NullRHI -nosound
```

The default output is `<Project>/lhat-host.json`, beside the `.uproject` file.
Override it with `-Output="Saved/Host API/lhat-host.json"` on the commandlet, or the console's
optional quoted argument. **Relative paths use the project directory**, not
the shell's working directory or the plugin directory. Parent directories
are created; an existing generated file is replaced with UTF-8 JSON without
a BOM. Read-only destinations are not forcibly overwritten. The commandlet
returns a nonzero exit code on failure; `-Help` prints its usage.

The commandlet runs UE without an editor window. It is not a switch for the
generic `lhat` CLI: that executable has no UE registrations to export. The
`LhatEditor` module supplies both entry points and is excluded from game builds.
The C++ `FLhatProgram::GetHostApiJson()` API also returns the UTF-8 payload.

Place the JSON at the project root visible to the LSP. The default works when
opening the UE project directory, including its `Script/` sources. If editing
this plugin's examples instead, use `-Output="Plugins/lhat-UE/lhat-host.json"` to export into this
repository; the game's Script directory is outside the plugin workspace.
Regenerate after changing native registrations and rebuilding the plugin.
The JSON describes only the currently exposed API, not every UE API, and is
not the binary signature table used by a future VM-only Shipping build.

## Tests

Build the project's Development Editor target, then run:

```powershell
.\Scripts\TestLhat.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject
```

The `Lhat.Runtime.*` UE automation tests run headlessly, covering component
lifecycle, independent instances, both GCs, destroyed-Actor access, path
validation, parameter defaults/overrides, native calls and execution budgets.
`Lhat.Editor.*` additionally checks host-API JSON, console/commandlet parity,
the project-root default, explicit output paths and preservation of read-only files on export failure.
With generated project modules installed, it also executes generated native
callbacks and checks that ordinary instance calls never enter `ProcessEvent`.
The offline generator tests run with `python Scripts/TestNativeGenerator.py`.
Tests create uniquely named temporary fixtures below the project's `Script/`
and remove their own files; they do not overwrite project scripts. The runner
fails for failed/missing tests and prints the log path under `Saved/Logs`.

## Remaining milestones

The opt-in [binding benchmark](Docs/BindingBenchmark.md) compares generated
direct Engine calls with a cached `ProcessEvent` bridge from actual Lhat loops.
Run `Scripts/BenchmarkBindings.ps1 -EngineRoot ... -ProjectPath ...` after
building the Development Editor target with generated native providers.

This is an editor/development binding slice, not a packaged-game pipeline.
Plugin-owned static Engine bindings and a precompiled-plugin packaging path
are available. The [automatic reflection bridge](Docs/DynamicBindings.md)
adds supported native/Blueprint functions and property accessors when no
static callback exists. It also contributes to host API exports; use
`-run=LhatDumpHostApi -BindingReport` for dispatch and exclusion details.
General structs/containers, latent/delegate integration, and a cooked native
callable-policy catalog remain separate milestones.
Even `BuildLhat.ps1 -Configuration Shipping` currently builds with the front
end. Raw source staging, a UE-aware compile commandlet, signature-table export,
VM-only Shipping linkage and a packaged-game smoke test remain to be added
before production packaging. They must share the registrations used
by the runtime; the generic L^ CLI does not know the `ue` host module.

## Updating the L^ revision

```powershell
git -C LhatCore fetch origin main
git -C LhatCore checkout origin/main
git add LhatCore
```

Re-run `BuildLhat.ps1` after changing the submodule revision, then commit the
updated gitlink in this repository. Keep binding changes in `lhat-UE`; changes
to the language core belong in the L^ repository.
