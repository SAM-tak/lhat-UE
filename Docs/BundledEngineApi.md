# Plugin-owned precompiled Engine bindings

The common Engine acceleration set is compiled directly into `Lhat`, not into
`LhatGeneratedRuntime` in a user's project. The registration is unconditional
and independent of module startup order: gather bundle types + optional
provider types, create shared tags, register the bundle, then register project
supplements. No `LhatExpose` annotations are required.

This is the middle tier of the agreed design, not a complete reflection
implementation. The profile chooses which APIs get precompiled acceleration;
it is **not** the exposure policy for the [automatic dynamic bridge](DynamicBindings.md).

## Initial set

18 root Engine classes: KismetMathLibrary, KismetStringLibrary,
KismetSystemLibrary, GameplayStatics, Actor, Pawn, Character, Controller,
PlayerController, ActorComponent, SceneComponent, PrimitiveComponent,
MovementComponent, PawnMovementComponent, CharacterMovementComponent,
MeshComponent, StaticMeshComponent and SkeletalMeshComponent.

All their supported native Blueprint-callable members are included, using the
existing generator's public-access/export checks and codecs. The snapshot
currently yields **1,136 Runtime + 16 WITH_EDITOR functions** and 68 additional
native types (including necessary signature types and their bases). The
compatibility Object/Actor/Vector bindings and other math value types are added
as before. Unsupported out/ref, general structs/containers/enums, FText,
latent, wildcard, event/RPC signatures remain excluded with reasons; see
`Bindings/BundledEngineApi.generated.json`. The 16 Editor-only callbacks are
compile-guarded, not merely hidden from host JSON in games.

The bundle depends only on built-in Engine runtime modules, not optional UE
plugins or the example project. Headers of signature types can require core
Engine modules such as AudioMixer/PhysicsCore; these are recorded explicitly.
Generated callbacks call C++ methods directly and do not use `ProcessEvent`.

Names stay the same, for example:

```text
ue.Engine.KismetMathLibrary.Abs
ue.Engine.KismetMathLibrary.VSize
ue.Engine.KismetStringLibrary.Concat_StrStr
ue.Actor.SetActorTickEnabled
ue.Engine.SceneComponent.SetRelativeScale3D
ue.Actor.GetActorLabel                 # Editor only
```

`lhat-host.json` is still generated from actual program registrations, so it
automatically includes the bundle plus any installed project supplements.
Its default destination remains `<Project>/lhat-host.json`.

## Blueprint-only consumers

Copy the packaged `Lhat/` folder to `<Project>/Plugins/Lhat/` and enable it.
Keep the package's `Binaries`, `Intermediate/Build`, `Build`, `Bindings` and
`Source` folders together. `Source` here belongs to the plugin; there is no
requirement to add C++ or a `Source` directory to the project itself. Opening
the Editor, running the component and exporting the host API require no
binding generator or local Core build.

Packages carry `Build/LhatPrecompiled.json`, enabling UBT's `bUsePrecompiled`
for their modules. This is not present in the maintainer/source checkout.
Do not remove the precompiled manifests/object files, or pretend a binary
package is compatible with another engine build. Package with each supported
UE version, platform and configuration; a local source-engine package is not
automatically compatible with Epic's Launcher binaries.

Raw Lhat sources remain outside Content, under `<Project>/Script/`. **Game
packaging of those files, VM-only Shipping and a packaged-game runtime smoke
test remain separate work.** Precompiling Development/Shipping plugin objects
does not validate a shipped game or promise that UE never needs a native
toolchain/temp target when packaging Blueprint projects with code plugins.

## Maintainer generation and project migration

Edit `Bindings/BundledEngineApi.json` to change the acceleration profile.
Use a header-aware Editor UHT snapshot from the existing export pipeline:

```powershell
python .\Scripts\GenerateBundledBindings.py --snapshot C:\Snapshot\UHT\Lhat
```

This writes tracked, reproducible C++ under `Source/Lhat/Private/Bundled/` and
the API/dependency manifest under `Bindings/`. The ownership manifest refuses
to overwrite edited files and removes only verified, obsolete generated files.
Consumers never run this generator.

If a project already uses optional `LhatGeneratedRuntime`/`LhatGeneratedEditor`,
run `GenerateNativeBindings.ps1` again and rebuild. It subtracts bundled
functions rather than registering duplicates. A signature/target mismatch is
an error requiring bundle regeneration, not a silent fallback. Declaration of
the same UClass by multiple providers shares its existing host tag; different
module/type names for the same UClass are rejected.

## Build a distributable plugin

```powershell
.\Scripts\PackageLhat.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -OutputDirectory C:\Build\LhatWin64
```

By default this builds Win64 Development Editor and precompiles UnrealGame
Development + Shipping via UE's `BuildPlugin`. `-EditorOnly` skips game
targets and marks the result so game-target builds fail with a clear message.
Optional `-OutputDirectory` must name a **nonexistent** directory.

The script rebuilds the current Core, stages only release inputs (no `.git`,
project-generated modules, submodule checkout or old intermediate files), and
copies first-party Core libs, decorated headers and export directives to
`Build/LhatCore/Win64/Native`. It includes Lhat/cJSON notices. All compiler/API
state still lives in the one Lhat DLL in modular builds.

Default output is `Saved/Packages/<timestamp>/Lhat` below the plugin checkout.
The sibling `Input` and `HostProject` directories are retained for diagnostics,
not for distribution. Existing output is never cleared. The final package
includes binaries and the precompiled manifests/objects required by UBT;
binary artifacts are not committed to git.

Verify a package in a new project with no C++ source and no generated modules:

```powershell
.\Scripts\TestPackagedLhat.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -PluginPackage C:\Package\Lhat
```

The script copies the package to a uniquely named Blueprint-only fixture with
default Engine plugins disabled and only Lhat explicitly enabled. It exports
host JSON with the ordinary commandlet, runs the UE tests and requires
the bundled Engine integration test to pass. No project compilation is invoked.
The new test exercises math, owning strings, component mutation, Actor and
Editor calls, checks bypass of `ProcessEvent`, and repeats after Lhat GC.
Fixtures and logs are retained for inspection.

Offline regressions:

```powershell
python .\Scripts\TestBundledBindings.py
python .\Scripts\TestNativeGenerator.py
python .\Scripts\TestCoreApi.py
```

## Verified on 2026-09-14 (before automatic dynamic bindings)

UE 5.8.2 source build, Win64/MSVC 14.44, LhatCore
`4fc68d686600aee95002bac5eefc69f1ac2668ff`:

- `BuildPlugin`: Editor Development DLLs and UnrealGame Development/Shipping
  precompiled objects all built successfully; both game manifests and their
  referenced objects are present. The Editor modules match the engine BuildId.
- FirstPersonTemplate with optional generated supplements: all 13 UE tests pass.
- Fresh Blueprint-only project, using only the copied binary package: 11 UE
  tests pass, including BundledEngineBindings; the two project-generated-provider
  tests explicitly skip because no such modules are installed. No project C++
  compilation, Core checkout or binding generation is invoked.
- Its project-root `lhat-host.json`: 77 types / 1,161 functions / 186,229 bytes
  (the 1,152 bundled functions plus the existing nine compatibility functions).
- Offline tests: 6 bundle + 17 native generator + 4 Core API tests pass.

The runtime Editor DLL is 1,613,824 bytes. The complete uncompressed package is
about 464 MB, including PDBs and both configurations' precompiled object files;
that is not the size of the runtime DLL or host JSON. Game execution and raw
script staging remain unverified as described above.

An initial fixture using all default Engine plugins stopped at an unrelated
out-of-date EditorTelemetry binary in this local source engine. The isolated
fixture avoids that dependency and also verifies that Lhat does not require
optional Engine plugins; no Engine or existing project configuration was changed.
