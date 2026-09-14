# Static versus cached reflection bindings

## Measured result — 2026-09-13

UE 5.8.2, Win64 Development Editor, NullRHI, AMD Ryzen 5 2600, Windows 11.
Both UE C++ binding paths use MSVC 19.44.35228 (toolset 14.44.35228); both
share the same Lhat Core DLL instance. Core's separate CMake Release build
reports MSVC 19.51.36256.0; pinned revision is
`20008749c496afd73b7af1223fdcdb230eb402d0`. This is not a compiler comparison.

Main repetition: 500,000 iterations per sample, 11 measured pairs after four
warmup pairs. Times below are microseconds per iteration, using medians:

| Native Engine API | Static µs | Cached µs | Cached/static | Paired extra µs |
| --- | ---: | ---: | ---: | ---: |
| Abs | 0.406 | 1.243 | 3.06× | 0.809 |
| VSize | 0.674 | 1.581 | 2.34× | 0.912 |
| Multiply_VectorFloat (+ result.X) | 0.988 | 1.921 | 1.94× | 0.938 |
| Len | 0.644 | 1.638 | 2.54× | 0.980 |
| Concat_StrStr | 1.295 | 2.415 | 1.86× | 1.096 |

The extra column is the median of paired differences, not the difference of
the two medians. Relative median absolute deviations are about 2–5% in this
run. An earlier 200,000-iteration / 11-sample / three-warmup run gave
1.83–2.79× and 0.92–1.26 µs extra. This is a normal desktop, without CPU
affinity/power tuning or an isolated benchmark environment: use the range,
not the last decimal place, for design decisions. All checksums passed.

Raw samples and environment details are retained in
[200k run](Benchmarks/2026-09-13-msvc-200k.json) and
[500k run](Benchmarks/2026-09-13-msvc-500k.json). Full logs and the generated
Lhat inputs remain under the original project's
`Saved/LhatBenchmarks/20260913-231315-847` and `20260913-231528-410`.

For these small calls, 10,000 invocations would imply roughly 8–13 ms of
additional work at the observed paired deltas (an extrapolation, not a frame
benchmark). This supports the three-tier design: broad dynamic availability,
with direct bindings for genuinely hot APIs. Blueprint-authored function
bodies and packaged Shipping still require separate measurements.

## Reproduce

Build the project's complete Win64 Development Editor target first, with the
generated native providers installed. The benchmark does not generate bindings
or change compiler settings. From this plugin directory:

```powershell
.\Scripts\BenchmarkBindings.ps1 -EngineRoot C:\Path\To\UnrealEngine `
    -ProjectPath C:\Path\To\Project\Project.uproject `
    -Iterations 200000 -Samples 11 -Warmups 3
```

The headless `LhatBenchmarkBindings` commandlet also accepts these arguments
directly, plus `-Output=<json path>`. Existing result files are not overwritten.
The runner creates a unique directory under `Saved/LhatBenchmarks` containing
raw JSON samples, the UE log, and all ten actual Lhat source files. Failure to
compile, execute, validate results, or save a complete report is an error exit.
Benchmark aliases exist only in its own program, never in the normal host API.

## What is measured

Five existing native Engine UFUNCTIONs: `KismetMathLibrary.Abs`, `VSize`,
`Multiply_VectorFloat`, and `KismetStringLibrary.Len`, `Concat_StrStr`.

- Static: actual registered generated C++ callbacks. The historical measurements
  above used `LhatGeneratedRuntime`; after the bundled Engine feature, these
  same APIs come from the plugin's Lhat DLL instead. The harness works without
  project-generated modules; new results must not be treated as a rerun with
  identical Core revision, compiler settings or DLL placement.
- Cached: one shared callback in `LhatEditor` calling the same UFUNCTION via
  `UObject::ProcessEvent`. UFunction, native class CDO, parameter offsets,
  codecs and initialization/destruction plans are resolved at registration.
- Cached parameters use a separate aligned stack frame per invocation. There
  is no parameter-buffer heap allocation, shared reusable frame, per-call
  function-name search or property-type discovery. Nontrivial values are
  initialized/destroyed correctly. String conversions still allocate.
- Both paths use the same Lhat native conversion helpers and signatures.
  Source pairs differ only in the called member name (`BenchCached_` prefix).
- The timed region is **one complete Lhat loop**, called from C++ once per
  batch, divided by its iteration count. It includes VM dispatch, loop work,
  codecs and the native function body, not merely ProcessEvent dispatch.
  Vector multiplication includes reading the result's `.X` member. One
  `ue.MakeVector(3, 4, 0)` call precedes each batch's loop in both modes; its
  cost is amortized over the batch. The stack-only vector is not captured.
- Setup, module loading, registration, parsing/compilation, VM installation
  and closure creation are excluded. Each mode has its own machine; closures
  are rooted. An explicit Lhat GC precedes each batch outside timing, while
  any automatic GC inside the loop remains included.
- Warmups run both modes. Measured pairs alternate execution order. Reports
  retain every sample, each mode's median, their ratio, and the median of
  paired differences. Checksums/final string values are verified every batch.

This is a warm native-API benchmark, **not** a measurement of Blueprint
bytecode, arbitrary object/Actor instance calls, cold discovery, editor hot
reload/reinstancing, or Shipping. The private bridge intentionally supports
only the tested synchronous static native signature subset. It must not be
promoted directly to the production Blueprint bridge: that needs invalidation
on Blueprint recompilation, UObject lifetimes, instance calls, more codecs,
and explicit policies for out/ref, latent, RPC and wildcard functions.

Do not interpret the ratios as the whole game's slowdown or as a universal
cost for all Unreal APIs. Tiny inline math particularly benefits from direct
C++ binding. More substantial function bodies can dominate both paths.

## Windows compiler selection

UBT supports `clang-cl`; Windows does not force the MSVC compiler frontend.
For example, append the following to a normal UBT/Build.bat target command
after installing the selected version:

```text
-Compiler=Clang -CompilerVersion=20.1.8 -NoClangLinker
```

`-NoClangLinker` explicitly keeps Microsoft's linker; choosing Clang's frontend
does not imply replacing Microsoft's SDK, C++ ABI, CRT or standard library.
The local UE 5.8.2 source defaults `bAllowClangLinker` to true, so specify this
option if the intent is to change only the compiler. A compiler switch can
invalidate engine/PCH/module build artifacts in this source-built engine;
it is not an isolated per-plugin speed switch.

As inspected on 2026-09-13:

- Epic's UE 5.8 release notes list the Windows build farm as VS 2022 17.14,
  MSVC toolset 14.44.35207; developer recommendation is MSVC 14.50.
- The local `Engine/Config/Windows/Windows_SDK.json` prefers Clang 20.1.8,
  with minimum 18.1.8. MSVC 14.44 headers additionally require Clang 19+.
- This PC has VS-bundled Clang 22.1.3 under
  `VC/Tools/Llvm/x64/bin/clang-cl.exe`, even though it is not on PATH. UBT's
  discovery code searches that location. 22.1.3 is outside this UE revision's
  preferred ranges; compatibility has not been build-tested here.
- `BuildLhat.ps1` builds the C language core separately with CMake/MSVC.
  A UBT `-Compiler=Clang` flag does not change that C build. Switching Core
  requires a separate CMake build directory/toolchain (e.g. LLVM/ClangCL),
  then rebuilding the UE consumers against the resulting artifacts. Do not
  reuse a CMake cache created with a different compiler.

No compiler configuration or Core source was changed for this benchmark.
Compiler quality is workload-dependent; a Clang/MSVC comparison is a separate
experiment from static/reflection dispatch.

Sources: [Epic Clang on Windows](https://dev.epicgames.com/documentation/unreal-engine/use-clang-to-build-microsoft-platforms-in-unreal-engine),
[UE 5.8 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes),
and the checked-out UE 5.8.2 `Windows_SDK.json`, `UEBuildWindows.cs`,
`MicrosoftPlatformSDK.cs`, `VCEnvironment.cs`. The Clang overview still carries
an old UE 5.3 version note; use this checkout's SDK rules for version selection.
