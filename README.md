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

`FLhatModule` installs Unreal's `FMemory` as the L^ allocator during module
startup. `FLhatProgram` is the initial C++ host wrapper: it confines script
file loading to a caller-specified root directory and exposes check, compile,
machine creation and installation. The public `lhat.h` API remains available
for the next binding layer (UObject/Blueprint types and native registrations).

## Updating the L^ revision

```powershell
git -C LhatCore fetch origin main
git -C LhatCore checkout origin/main
git add LhatCore
```

Re-run `BuildLhat.ps1` after changing the submodule revision, then commit the
updated gitlink in this repository. Keep binding changes in `lhat-UE`; changes
to the language core belong in the L^ repository.
