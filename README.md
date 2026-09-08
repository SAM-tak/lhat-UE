# lhat-UE

Runtime binding for embedding [L^](https://github.com/SAM-tak/lhat) in Unreal
Engine projects.

## Current development target

- Unreal Engine 5.8.2
- Windows 64-bit
- Visual Studio 2022 or 2026 with the C++ game-development workload
- CMake 3.25 or later

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

For a debug build, use `-Configuration Debug`. All other Unreal configurations
link the Release variant.

## Using it in an Unreal project

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
GetActorLocation, SetActorHiddenInGame and IsActorTickEnabled use the reflected
`UFunction` bridge with initialized parameter storage and reflected return
offsets. The bridge has a deliberately small, explicit native-method allowlist;
arbitrary Blueprint calls, out parameters, containers, latent calls, delegates,
RPCs and script-defined UClasses are not supported yet.

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

## Tests

Build the project's Development Editor target, then run:

```powershell
.\Scripts\TestLhat.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject
```

The `Lhat.Runtime.*` UE automation tests run headlessly, covering component
lifecycle, independent instances, both GCs, destroyed-Actor access, path
validation, parameter defaults/overrides, reflected calls and execution budgets.
Tests create uniquely named temporary fixtures below the project's `Script/`
and remove their own files; they do not overwrite project scripts. The runner
fails for failed/missing tests and prints the log path under `Saved/Logs`.

## Remaining milestones

This is an editor/development binding slice, not a packaged-game pipeline.
Even `BuildLhat.ps1 -Configuration Shipping` currently builds with the front
end. Raw source staging, a UE-aware compile commandlet, signature-table export,
VM-only Shipping linkage and a packaged-game smoke test remain to be added
before broad automatic API exposure. They must share the registrations used
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
