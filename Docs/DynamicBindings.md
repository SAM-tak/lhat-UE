# Automatic Blueprint API bindings

Each new `FLhatProgram` registers the loaded UE class hierarchy and its
supported Blueprint-callable functions. No `LhatExpose` metadata or project
C++ generation is required.

Registration order is:

1. Gather types from bundled/project providers and reflection.
2. Declare shared host tags and math value types.
3. Register bundled static callbacks and optional project static callbacks.
4. Register dynamic callbacks only for names not already registered.

The common dispatcher is selected at registration, not by searching a name on
every Lhat call. Function layouts, offsets, conversions, and out/ref positions
are cached. Instance calls cache the actual `UFunction` per concrete receiver
class, so native base signatures also dispatch to Blueprint overrides.
Static reflected functions use the declaring class's CDO. Dynamic calls use
`ProcessEvent`; bundled/project callbacks retain their direct C++ route.

## Names and discovery

Native classes keep the existing `ue.<NativeModule>.<Class>` names; Object and
Actor retain `ue.Object` and `ue.Actor`. Blueprint classes use the content
package path to avoid collisions, e.g. `/Game/Enemies/BP_Enemy.BP_Enemy_C` becomes
`ue.BP.Game.Enemies.BP_Enemy.BP_Enemy_C`. Keeping the package leaf also separates
PIE map classes from their originals. Ordinary names, including Japanese names, are
preserved; names containing punctuation are escaped with a reserved `_LH_`
UTF-8 hex spelling.

In the Editor, initialization discovers and loads Blueprint assets under
`/Game` and enabled project plugins' content roots. It does not explicitly
compile or save assets. Loaded classes from enabled Engine plugins and
unsaved/transient Blueprints are also included. Disabled plugins and
unloaded optional native modules are not force-loaded. Classes loaded after
a snapshot require a new program; there is no mutation of a checked program.
The cooked runtime uses the classes already loaded by UE.

The hierarchy also declares opaque classes needed for object identity and
signature references, including classes without callable members. This is
not a security sandbox: exposed native APIs have their usual engine effects.

## Supported values and calling convention

- Booleans (including bitfield properties), signed/unsigned integers,
  float/double, FString, FName.
- FText is converted to/from `string^`: this intentionally does **not**
  preserve localization keys/history.
- UObject references and UClass/TSubclassOf, with nil and class constraints.
- Vector, Vector2D, Rotator, Quat, Transform, LinearColor and Color.
- Enum-backed numeric parameters use `number^` with underlying storage range
  checks; named/nominal enum declarations are not implemented yet.
- Ordinary inputs, outputs, and mutable reference parameters.

Integers do not pass through double when the caller supplies an integer.
Overflow, fractional integer input, invalid names, expired object handles and
incompatible classes produce a Lhat failure before dispatch.

All reflected input pins are explicit, including WorldContext/default
parameters. Defaults and implicit WorldContext/DefaultToSelf injection are
not implemented. A pure-out pin is omitted from inputs; a mutable reference
pin is both an input and an output. Results are the UE return value first
(if any), followed by out/ref pins in declaration order:

```text
let^ success, outValue, outText = object.Method(input)
let^ updatedValue = object.ModifyByReference(originalValue)
```

Math values currently must be the **sole** result. Functions combining a math
return/out pin with other results (or returning several math pins) are excluded
as `hostvalue_in_multiple_results`: the pinned core has only one wide-result
scratch area and does not support math values inside multiple-result runs.

## Properties

Supported Blueprint-visible fields receive `Get_<Property>()` and, unless
read-only, `Set_<Property>(value)` methods. The core currently has no hostdata
property-registration API, so this does not add `object.Property` syntax.

Private/protected fields without UE's `AllowPrivateAccess`, custom native or
Blueprint getter/setter-backed fields, and
replicated-field setters are excluded rather than bypassing accessors or
replication bookkeeping. Accessor name collisions are reported, never
overwritten. UObject handles are still weak: passing an object to Lhat does
not transfer its UE lifetime to the script.

## Lifetimes, reload and limits

Calls are synchronous and game-thread-only. Each invocation has independent
aligned parameter storage; strings/text are constructed and destroyed on
every exit. Receivers and object inputs are held strongly for the call.
Descriptors/classes are retained by the program; the per-call path does not
rediscover property kinds. Frames are capped at 64 KiB, nesting at 64 calls.

Blueprint compilation, class replacement or native reload invalidates a
snapshot. Old dynamic calls fail with a request to recreate the Lhat program,
rather than using stale field offsets. Restart PIE/recreate the program after
editing Blueprints. Automatic live migration of running scripts/host tags is
not implemented. Static C++ callbacks retain their existing UE reload limits.

Not yet supported: general UStruct values (e.g. HitResult), arrays/maps/sets,
interfaces, soft/weak references, delegates, latent/custom-thunk/wildcard
nodes, RPCs, and private/protected functions. Unsupported signatures are
reported, not partially bound with missing pins.

**Cooked native reflection needs another step.** UE strips metadata used to
identify unsafe CustomThunk/latent/internal nodes. When `WITH_METADATA=0`,
unregistered native functions and reflected property accessors fail closed
with `native_metadata_unavailable`/`property_metadata_unavailable`.
This includes Blueprint-defined properties: their private/custom-accessor
metadata can also be stripped.
Static bindings remain available; supported loaded Blueprint bytecode
functions can use the bridge. A cook-time callable-policy catalog is needed
for general native dynamic dispatch in packaged games. Raw-script staging
and a packaged-game runtime test also remain separate work.

## Host API and diagnostics

`lhat-host.json` comes from the same complete program registrations, so
checking/completion and execution see the same supported static + dynamic
functions. The default stays the project root.

```powershell
UnrealEditor-Cmd.exe Project.uproject -run=LhatDumpHostApi -unattended -NullRHI
UnrealEditor-Cmd.exe Project.uproject -run=LhatDumpHostApi -BindingReport -unattended -NullRHI
```

The second command writes `<Project>/lhat-bindings.json` with dispatch counts,
paths, registered member names, and per-member exclusion reasons. Both accept
`-Output=...`; existing output is replaced atomically. The report is diagnostic
data, not a runtime manifest or an exposure allowlist.

Tests `Lhat.Editor.DynamicNativeBindings` and
`Lhat.Editor.DynamicBlueprintBindings` exercise the bridge using a private
unexported native fixture and real transient Blueprint bytecode, including
static precedence, out/ref values, properties, object/Class boundaries, GC,
Blueprint overrides and recompile invalidation. No test Blueprint is saved.

## Verified on 2026-09-14

UE 5.8.2 source build, Win64/MSVC 14.44, LhatCore
`4fc68d686600aee95002bac5eefc69f1ac2668ff` (unchanged by this feature):

- FirstPersonTemplate Editor: all 15 Lhat automation tests passed, including
  the two new dynamic tests. These also exercise FText and escaped Blueprint
  function names containing spaces. All 27 offline tooling tests passed.
- CLI host export, with the project's optional generated providers enabled:
  9,600 types / 16,367 functions / 4,223,264 bytes. The report contains 9,593
  reflected classes (the host export also has seven math value types), 6,509
  static routes, 9,851 dynamic routes, and 8,672 skipped member candidates
  (including 173 functions with math values in multiple results).
  Module-level compatibility functions are outside the reflected inventory.
- Host export commandlet work took 8.93 seconds, excluding Editor startup
  and including Blueprint discovery/loading. This is not per-call overhead.

These counts cover currently supported signatures, not the earlier complete
Blueprint API survey. The verification outputs are
`Saved/LhatDynamic-host.json` and `Saved/LhatDynamic-bindings.json`; the
project-root host file was not overwritten during verification.

### Binary-only project verification

The final package at `D:/LhatPackages/UE582-Dynamic-20260914-v3/Lhat` passed
BuildPlugin for Editor Development and Win64 Game Development/Shipping.
These are build/precompiled-output checks, not cooked-game execution tests.

`Scripts/TestPackagedLhat.ps1` copied only this package into a fresh Blueprint
project with no project Source, Modules, Target.cs, or generated providers.
Default Engine plugins were disabled for this minimal fixture. Host export
succeeded with 4,240 types / 8,281 functions / 1,933,834 bytes. Both dynamic
integration tests passed, including real Blueprint bytecode and recompile
invalidation. All 13 applicable tests passed; the two project-generator tests
reported that they were skipped because generated modules were absent.
The test did not compile any project C++.
