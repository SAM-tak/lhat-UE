# FirstPersonTemplate API size survey — 2026-09-13

The project-wide Blueprint API fits in a single JSON file of roughly **4.9 MB
for type declarations and UFUNCTION signatures**, or **11.3 MB including property
accessors**. Including tooltips and parameter names increases the latter to
21.1 MB. These are measured UTF-8 file sizes, not extrapolations from a sample.
MB below means 1,000,000 bytes.

## Results

| Inventory | UCLASS | UFUNCTION | UPROPERTY | Types + functions | Also property accessors | Also tooltips + parameter names |
|---|---:|---:|---:|---:|---:|---:|
| Runtime-classified modules, Blueprint API candidates | 3,588 | 13,621 | 19,281 | 4,007,179 B | 9,977,661 B | 18,258,948 B |
| Editor target, Blueprint API candidates | 4,407 | 16,702 | 20,639 | 4,870,805 B | 11,278,847 B | 21,107,611 B |
| Editor target, all reflected declarations (including non-Blueprint members) | 9,772 | 18,517 | 62,724 | 7,468,461 B | 28,534,542 B | 44,723,907 B |

Counts include Blueprint-generated classes. UFUNCTION counts include Blueprint
events (1,606 in the middle row); generated property accessors are counted
separately. The middle row contains 37,582 property accessors, 3,138 native
UStruct declarations and 1,348 native UEnum declarations in addition to UCLASS.
Dependencies are retained in the type table even in the functions-only file.
Inherited members are stored once on their declaring class, with a base-type
link, as supported by the existing host-JSON format.

The middle row's minified JSON is 10,374,865 bytes; deeply indented JSON would
be 13,704,768 bytes. Its minified gzip payload is 717,322 bytes, although gzip
is not an input format currently supported by the language server.

JSON decoding alone took a median of 83.01 ms for the 11.3 MB file in five
Python runs on this machine (Ryzen 5 2600). **This is not an Lhat LSP benchmark:**
type registration, checking, completion latency and language-server memory
use have not been measured.

## Collection scope

- Local engine: UE 5.8.2, Win64 Development Editor, FirstPersonTemplate.
- Native inventory: the project's UBT-generated Editor `.uhtmanifest`, not
  a scan of every installed plugin and not just the currently loaded UClasses.
  All 588 listed modules produced an export: 254 EngineRuntime, 2 GameRuntime,
  249 EngineEditor, 1 GameEditor, 40 EngineDeveloper and 42 EngineUncooked.
  Subsequent native-binding work confirmed that this build-header inventory can
  also contain dependencies from disabled plugins. The numbers above retain the
  original broad survey scope; the executable binding generator additionally
  checks the editor's actual enabled-plugin inventory.
- Those modules declare 9,442 UClasses, 17,198 functions, 8,490 structs and
  3,209 enums before applying the Blueprint filter.
- Blueprint inventory: all 330 saved standalone UBlueprint assets found in
  mounted content roots, loaded successfully with zero missing generated classes.
  Of these, 44 are in `/Game`, 30 in `/Engine`, and 256 in plugin content.
  For example, the enabled PCG content contributes 89 Blueprints and
  ControlRigModules contributes 60. Engine-default-enabled plugins are included.
- No map-embedded level scripts, unsaved editor changes or per-map reachability
  analysis. "Referenced by the project" here means available through its build
  target and mounted content, not only functions called by the current map.
- The Runtime row is a module classification filter, **not a Shipping-target
  or cooked-asset measurement**. Editor-only declarations inside runtime modules
  and Blueprint dependencies may still be present.

Blueprint candidates are functions carrying BlueprintCallable, BlueprintPure
or BlueprintEvent, excluding delegates and BlueprintInternalUseOnly functions;
and properties carrying BlueprintVisible, BlueprintAssignable or BlueprintCallable.
BlueprintType declarations, class inheritance and signature dependencies are
included. This is intentionally a broad static catalog; contextual Blueprint
access checks and custom K2-node behavior are not modeled by this measurement.

## What the generated JSON means

This is a **size experiment**, not additional executable UE bindings. The files
use the existing `strict / types / functions / annotations / bindings` envelope,
real declaration names, inheritance and signatures projected into Lhat spelling.
Native structs are represented as nominal hostdata placeholders. Containers,
delegates and other unresolved ABI mappings use explicit opaque nominal types;
their original C++ spelling is retained in `opaque-types.json` and the raw
inventory. Enum members and numeric values are included where available from UHT.

The functions-only variants exclude generated property getter/setter entries.
The full variants represent properties as getter/setter signatures, respecting
BlueprintReadOnly. This is a measurement convention, not a decided public
property-access syntax. Documentation keys are experimental extra fields.

Blueprint-referenced user-defined structs/enums have not been expanded: 68
referenced declarations in the Blueprint scenarios, and 72 in the all-reflected
scenario, are represented by named placeholders. The latter additionally includes
generated animation data structs. All such paths are listed in `summary.json`.
No selected function is dropped because its parameter type lacks a marshaller.
These approximations mean a final production binding format can differ in size.
No claim is made that these experimental declarations pass Lhat semantic loading.

The project's real `lhat-host.json` was not replaced. Its SHA-256 remains
`539EF905368373C29D1226C039D1C489DA44BB4B3E2E0C5F86F5141109B0A15A`.

## Files and reproduction

The measured snapshot on this workspace is under
`<Project>/Saved/LhatApiSurvey/20260913-4/`:

- `UHT/Lhat/*.json`: flat native inventory, one file per build-target module.
- `blueprints.json`: raw Blueprint declaration inventory.
- `Measured/summary.json`: exact counts, sizes, unresolved paths and module breakdown.
- `Measured/editor-blueprint/lhat-host.functions-only.json`: 4.9 MB variant.
- `Measured/editor-blueprint/lhat-host.json`: 11.3 MB variant.
- `Measured/editor-blueprint/lhat-host.documented.json`: 21.1 MB variant.
- Corresponding `runtime-blueprint` and `editor-all-reflected` directories.

Build the project's Editor target first so the survey commandlet is available.
Run the following from the project directory, choosing new snapshot/output names:

```powershell
$engine = 'C:\Users\Owner\UnrealEngine'
$project = (Resolve-Path './FirstPersonTemplate.uproject').Path
$snapshot = './Saved/LhatApiSurvey/new-snapshot'

./Plugins/lhat-UE/Scripts/ExportUhtForSurvey.ps1 `
    -EngineRoot $engine -ProjectPath $project `
    -ManifestPath './Intermediate/Build/Win64/FirstPersonTemplateEditor/FirstPersonTemplateEditor.uhtmanifest' `
    -OutputDirectory $snapshot

& "$engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" $project `
    -run=LhatSurveyBlueprintApi "-Output=$snapshot/blueprints.json" `
    -unattended -NullRHI -nosound -nosplash

py -3 ./Plugins/lhat-UE/Scripts/MeasureHostApi.py $snapshot `
    --blueprints "$snapshot/blueprints.json" --output "$snapshot/Measured"
```

The native script builds a small C# exporter against the local UE 5.8/.NET 10
assemblies and injects it into a copied manifest. All survey output goes into the
new snapshot directory; normal engine-generated files and plugin configuration
are not changed. The stock UHT Json exporter on this engine failed on cyclic
EngineClass references, which is why the survey serializes an explicit flat DTO.

The successful UHT collection took 19.85 seconds. Blueprint inspection reported
158.84 seconds, excluding some UE startup time; asset loading also warmed derived
data caches. No assets were saved. Collection time is distinct from LSP file-load
time and is a stronger reason to investigate incremental updates than file size
alone. API partitioning is not yet justified solely by these file sizes.
