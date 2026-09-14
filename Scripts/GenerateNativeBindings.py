#!/usr/bin/env python3
"""Generate direct C++ host callbacks from a target-specific UHT snapshot.

No exposure allowlist: unsupported signatures/access are recorded in the report.
Generated files are owned by a hash manifest; edited files are never overwritten.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


VERSION = 1
RUNTIME = "LhatGeneratedRuntime"
EDITOR = "LhatGeneratedEditor"
BUILTINS = {"/Script/CoreUObject.Object": ("ue", "Object"), "/Script/Engine.Actor": ("ue", "Actor")}
BUILTIN_FUNCTIONS = {("Object", "GetName"), ("Object", "IsValid"), ("Actor", "SetActorLocation"),
                     ("Actor", "SetActorHiddenInGame"), ("Actor", "IsActorTickEnabled")}
MATH = {"FVector": "Vector", "FVector2D": "Vector2D", "FRotator": "Rotator", "FQuat": "Quat",
        "FTransform": "Transform", "FLinearColor": "LinearColor", "FColor": "Color"}
SCALARS = {"bool": "bool^", "int8": "number^", "int16": "number^", "int32": "number^",
           "int64": "number^", "uint8": "number^", "uint16": "number^", "uint32": "number^",
           "uint64": "number^", "float": "number^", "double": "number^", "FString": "string^", "FName": "string^"}


def flags(record, key="flags"):
    return set(record.get(key, "").split(", "))


def ident(name):
    return bool(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name))


def quote(value):
    return json.dumps(value, ensure_ascii=True)


def is_editor(record):
    return record["module_type"] not in {"EngineRuntime", "GameRuntime"}


class Unsupported(Exception):
    pass


@dataclass
class Codec:
    cpp: str
    signature: str
    math: str | None = None
    reference: str | None = None

    def read(self, slot, variable):
        suffix = f", {quote(self.math)}" if self.math else ""
        return f"LhatUEBindings::Read{'Value' if self.math else ''}(M, B, Args[{slot}], {variable}{suffix})"

    def write(self, variable):
        suffix = f", {quote(self.math)}" if self.math else ""
        return f"LhatUEBindings::Write{'Value' if self.math else ''}(M, B, {variable}, Answers[0]{suffix})"


class Generator:
    def __init__(self, directory, context=None):
        self.context = context or {"modules": {}}
        self.types = {}
        self.sources = []
        for path in sorted(directory.glob("*.json")):
            document = json.loads(path.read_text(encoding="utf-8-sig"))
            if not all(k in document for k in ("module", "module_type", "types")):
                raise ValueError(f"Not a UHT snapshot: {path}")
            self.sources.append({"module": document["module"], "sha256": digest(path.read_bytes())})
            for entry in document["types"]:
                self.types[entry["path"]] = dict(entry, module=document["module"], module_type=document["module_type"])
        if not self.types:
            raise ValueError("No native types found")
        self.registered = set(BUILTINS) & self.types.keys()
        # Some public headers include a sibling module whose dependency was marked
        # private by its owner. Resolve these public include roots for external TUs.
        self.include_roots = {}
        parents = set()
        for record in self.types.values():
            header = record.get("header")
            if header:
                base = Path(header["file"])
                for _ in Path(header["relative"]).parts:
                    base = base.parent
                parents.add(base.parent)
        for parent in sorted(parents):
            for sibling in parent.glob("*/*.Build.cs"):
                self.include_roots[re.sub(r"(?i)\.build\.cs$", "", sibling.name)] = sibling.parent / "Public"
        self.include_modules = defaultdict(set)
        self.nonpublic_includes = set()
        for module, root in self.include_roots.items():
            for directory, _, filenames in os.walk(root):
                for filename in filenames:
                    if filename.endswith((".h", ".hpp", ".inl")):
                        relative = (Path(directory) / filename).relative_to(root).as_posix()
                        self.include_modules[relative].add(module)
            for kind in ("Internal", "Private"):
                for directory, _, filenames in os.walk(root.parent / kind):
                    for filename in filenames:
                        self.nonpublic_includes.add((Path(directory) / filename).relative_to(root.parent / kind).as_posix())
        self.public_header_reasons = {}
        self.editor_modules = {r["module"] for r in self.types.values() if is_editor(r)}
        self.editor_modules.update(name for name, root in self.include_roots.items() if "Editor" in root.parts)
        self.header_dependencies = {}
        self.functions = {RUNTIME: [], EDITOR: []}
        self.rejected = []
        self.rejected_types = []
        self.provided_functions = []

    def type_name(self, record):
        return BUILTINS.get(record["path"], ("ue." + record["module"], record["name"]))

    def module_reason(self, module):
        info = self.context["modules"].get(module)
        if info and not info["enabled"]:
            return "plugin_disabled:" + info["plugin"]
        if info and info.get("explicitly_loaded"):
            return "plugin_explicitly_loaded:" + info["plugin"]
        return None

    def class_reason(self, record):
        if record["path"] in BUILTINS:
            return None
        if reason := self.module_reason(record["module"]):
            return reason
        if record["kind"] != "class" or "Interface" in flags(record):
            return "interface_or_nonclass"
        if record.get("define_scope", "None") != "None":
            return "conditional_class"
        if not ({"RequiredAPI", "MinimalAPI"} & flags(record)):
            return "class_not_exported"
        if not ident(record["cpp_name"]) or not ident(record["name"]):
            return "class_cpp_name"
        header = record.get("header")
        if not header:
            return "missing_header_information: regenerate UHT snapshot"
        parts = header["relative"].replace("\\", "/").split("/")
        if parts[0] not in {"Public", "Classes"}:
            return "nonpublic_header"
        if not header["include"] or not Path(header["file"]).is_file():
            return "missing_header"
        return self.header_reason(Path(header["file"]))

    def header_reason(self, path):
        if path in self.public_header_reasons:
            return self.public_header_reasons[path]
        self.public_header_reasons[path] = None  # Break include cycles.
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.MULTILINE):
            matches = self.include_modules.get(include, set())
            if not matches and include in self.nonpublic_includes:
                self.public_header_reasons[path] = "header_requires_nonpublic_include:" + include
                break
            if len(matches) == 1:
                module = next(iter(matches))
                if reason := self.module_reason(module):
                    self.public_header_reasons[path] = "header_requires_" + reason
                    break
                other = self.include_roots[module] / include
                reason = self.header_reason(other)
                if reason:
                    self.public_header_reasons[path] = reason
                    break
        return self.public_header_reasons[path]

    def codec(self, prop):
        if prop.get("array_dimension"):
            raise Unsupported("fixed_array")
        if prop.get("define_scope", "None") not in {"None", "Editor"}:
            raise Unsupported("conditional_parameter")
        if "OutParm" in flags(prop) and "ConstParm" not in flags(prop) and "ReturnParm" not in flags(prop):
            raise Unsupported("out_or_mutable_reference")
        cpp = prop["cpp_type"].strip()
        cpp = re.sub(r"^const\s+", "", cpp).removesuffix("&").strip()
        if cpp in SCALARS:
            return Codec(cpp, SCALARS[cpp])
        if cpp in MATH:
            return Codec(cpp, "ue." + MATH[cpp], math=MATH[cpp])
        refs = prop.get("references", [])
        if prop["property_class"] == "ObjectProperty" and len(refs) == 1 and re.fullmatch(r"[AU][A-Za-z_0-9]+\s*\*", cpp):
            record = self.types[refs[0]]
            reason = self.class_reason(record)
            if reason:
                raise Unsupported("object_type:" + reason)
            module, name = self.type_name(record)
            # UObject parameters/returns are nullable; receivers are checked separately.
            return Codec(cpp, module + "." + name + "|nil^", reference=refs[0])
        raise Unsupported("codec:" + prop["property_class"] + ":" + cpp)

    def function(self, owner, fn):
        reason = self.class_reason(owner)
        if reason:
            raise Unsupported(reason)
        ff, ef, meta = flags(fn), flags(fn, "export_flags"), fn.get("metadata", {})
        if "Native" not in ff:
            raise Unsupported("not_native")
        if "Public" not in ff:
            raise Unsupported("nonpublic_function")
        if {"Event", "Net", "Delegate", "MulticastDelegate"} & ff:
            raise Unsupported("event_rpc_or_delegate")
        if "CustomThunk" in ef or "CustomThunk" in meta:
            raise Unsupported("custom_thunk")
        if {"Latent", "CustomStructureParam", "ArrayParm", "DeterminesOutputType"} & meta.keys():
            raise Unsupported("special_blueprint_call_semantics")
        if "RequiredAPI" not in flags(owner) and not {"RequiredAPI", "Inline"} & ef:
            raise Unsupported("function_not_exported")
        scope = fn.get("define_scope", "None")
        if scope not in {"None", "Editor"}:
            raise Unsupported("conditional_function:" + scope)
        if not ident(fn["name"]):
            raise Unsupported("function_name")
        if owner["path"] in BUILTINS and (owner["name"], fn["name"]) in BUILTIN_FUNCTIONS:
            raise Unsupported("existing_builtin")
        inputs, result = [], None
        for prop in fn["parameters"]:
            codec = self.codec(prop)
            if "ReturnParm" in flags(prop):
                result = codec
            else:
                inputs.append(codec)
        if len(inputs) + ("Static" not in ff) > 30:
            raise Unsupported("argument_count")
        dependencies = [owner] + [self.types[c.reference] for c in inputs + ([result] if result else []) if c.reference]
        editor = scope == "Editor" or any(p.get("define_scope") == "Editor" for p in fn["parameters"]) or any(is_editor(r) for r in dependencies)
        group = EDITOR if editor else RUNTIME
        signature_inputs = ([] if "Static" in ff else ["self^"]) + [c.signature for c in inputs]
        signature = ("f^" if "BlueprintPure" in ff else "p^") + ", ".join(signature_inputs)
        if result:
            signature += " -> " + result.signature
        signature += ";"
        self.functions[group].append(dict(owner=owner, function=fn, inputs=inputs, result=result,
                                          signature=signature, dependencies=dependencies))
        for record in dependencies:
            self.registered.add(record["path"])

    def blueprint_type(self, record, seen=None):
        seen = set() if seen is None else seen
        if record["path"] in seen:
            return False
        seen.add(record["path"])
        meta = record.get("metadata", {})
        if "BlueprintType" in meta:
            return meta["BlueprintType"].lower() != "false"
        if meta.get("IsBlueprintBase", "false").lower() != "false":
            return True
        base = self.types.get(record.get("base_path"))
        return bool(base and self.blueprint_type(base, seen))

    def collect(self, roots=None):
        for record in sorted(self.types.values(), key=lambda r: r["path"]):
            if roots is not None and record["path"] not in roots:
                continue
            if record["kind"] != "class":
                continue
            class_reason = self.class_reason(record)
            blueprint_type = self.blueprint_type(record)
            if class_reason and (blueprint_type or any({"BlueprintCallable", "BlueprintPure", "BlueprintEvent"} & flags(f) for f in record["functions"])):
                self.rejected_types.append({"path": record["path"], "reason": class_reason})
            if not class_reason and blueprint_type:
                self.registered.add(record["path"])
            for fn in record["functions"]:
                if not {"BlueprintCallable", "BlueprintPure", "BlueprintEvent"} & flags(fn):
                    continue
                try:
                    self.function(record, fn)
                except Unsupported as error:
                    self.rejected.append({"path": fn["path"], "reason": str(error)})
        # Include all accessible bases, even when the base itself has no BP methods.
        for path in sorted(self.registered.copy()):
            record = self.types[path]
            while record.get("base_path") in self.types:
                record = self.types[record["base_path"]]
                if not self.class_reason(record):
                    self.registered.add(record["path"])

    def exclude_bundled(self, manifest):
        """Project callbacks supplement the plugin; never register a second copy."""
        provided = {e["path"]: e for e in manifest["generated_functions"]}
        for group in (RUNTIME, EDITOR):
            remaining = []
            for entry in self.functions[group]:
                path = entry["function"]["path"]
                if path not in provided:
                    remaining.append(entry)
                    continue
                if (provided[path]["signature"], provided[path]["group"]) != (entry["signature"], group):
                    raise ValueError(f"Bundled API signature/target changed: {path}; regenerate the plugin's bundled bindings first")
                self.provided_functions.append(path)
            self.functions[group] = remaining

    def emit_callback(self, entry, index):
        owner, fn, inputs, result = (entry[k] for k in ("owner", "function", "inputs", "result"))
        static = "Static" in flags(fn)
        out = [f"static void Call{index}(LhatMachine* M, void* Context, const LhatValue* Args, size_t Count, LhatValue* Answers, int* AnswerCount)", "{",
               "    check(IsInGameThread());", "    auto& B = *static_cast<FLhatBindings*>(Context);",
               f"    if (Count != {len(inputs) + (not static)}) {{ LhatUEBindings::Fail(M, \"Native argument count mismatch\"); return; }}"]
        if not static:
            out += [f"    auto* Self = static_cast<{owner['cpp_name']}*>(B.ReadObject(M, Args[0], {owner['cpp_name']}::StaticClass()));",
                    "    if (!Self) return;"]
        for i, codec in enumerate(inputs):
            out += [f"    {codec.cpp} Arg{i}{{}};", f"    if (!{codec.read(i + (not static), f'Arg{i}')}) return;"]
        call = (owner["cpp_name"] + "::" if static else "Self->") + fn["name"] + "(" + ", ".join(f"Arg{i}" for i in range(len(inputs))) + ")"
        if result:
            out += [f"    const auto Result = {call};", f"    if ({result.write('Result')}) *AnswerCount = 1;"]
        else:
            out += [f"    {call};"]
        return "\n".join(out + ["}"])

    def include_dependencies(self, header):
        path = header["file"]
        if path not in self.header_dependencies:
            result = set()
            text = Path(path).read_text(encoding="utf-8-sig", errors="replace")
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.MULTILINE):
                matches = self.include_modules.get(include, set())
                if len(matches) == 1:
                    result.update(matches)
            self.header_dependencies[path] = result
        return self.header_dependencies[path]

    def emit(self):
        files = {}
        report = {"version": VERSION, "sources": self.sources, "context": self.context, "groups": {}, "skipped": self.rejected, "skipped_types": self.rejected_types,
                  "provided_by_bundle": self.provided_functions}
        for group in (RUNTIME, EDITOR):
            modules = defaultdict(lambda: {"types": [], "functions": []})
            for path in sorted(self.registered - BUILTINS.keys()):
                record = self.types[path]
                if is_editor(record) == (group == EDITOR):
                    modules[record["module"]]["types"].append(record)
            for entry in self.functions[group]:
                modules[entry["owner"]["module"]]["functions"].append(entry)
            dependencies = {"Core", "CoreUObject", "Engine", "Lhat"}
            editor_dependencies = set()
            if group == EDITOR:
                dependencies.add(RUNTIME)
            units = []
            for module, content in sorted(modules.items()):
                # Bound translation unit size, including type-only registration units.
                chunks = [content["functions"][i:i+80] for i in range(0, len(content["functions"]), 80)] or [[]]
                for chunk_index, chunk in enumerate(chunks):
                    key = f"{module}_{chunk_index}"
                    units.append(key)
                    types = content["types"] if chunk_index == 0 else []
                    required = {r["path"]: r for r in types}
                    for entry in chunk:
                        required.update((r["path"], r) for r in entry["dependencies"])
                    headers = {r["header"]["include"] for r in required.values()}
                    dependencies.update(r["module"] for r in required.values())
                    for record in required.values():
                        extra = self.include_dependencies(record["header"])
                        if group == RUNTIME:
                            editor_dependencies.update(extra & self.editor_modules)
                            extra = extra - self.editor_modules
                        dependencies.update(extra)
                    lines = ["// Generated by GenerateNativeBindings.py; do not edit.", '#include "LhatNativeBindings.h"',
                             "PRAGMA_DISABLE_DEPRECATION_WARNINGS"]
                    lines += ['#include ' + quote(h) for h in sorted(headers)]
                    lines += ["namespace {", *[self.emit_callback(e, i) for i, e in enumerate(chunk)], "}"]
                    lines += [f"void Gather_{group}_{key}(TArray<FLhatNativeType>& Types)", "{"]
                    lines += [f"    Types.Add({{{r['cpp_name']}::StaticClass(), {quote(self.type_name(r)[0])}, {quote(self.type_name(r)[1])}}});" for r in types]
                    lines += ["}", f"bool Register_{group}_{key}(LhatProgram* P, FLhatBindings& B)", "{"]
                    for i, entry in enumerate(chunk):
                        module_name, type_name = self.type_name(entry["owner"])
                        fn = entry["function"]
                        lines.append(f"    if (!lhat_register_member(P, {quote(module_name)}, {quote(type_name)}, {quote(fn['name'])}, {quote(entry['signature'])}, Call{i}, &B)) return false;")
                    lines += ["    return true;", "}", "PRAGMA_ENABLE_DEPRECATION_WARNINGS", ""]
                    files[f"Source/{group}/Private/{key}.cpp"] = "\n".join(lines)
            lines = ['// Generated by GenerateNativeBindings.py; do not edit.', '#include "LhatNativeBindings.h"', '#include "Modules/ModuleManager.h"']
            for key in units:
                lines += [f"void Gather_{group}_{key}(TArray<FLhatNativeType>&);", f"bool Register_{group}_{key}(LhatProgram*, FLhatBindings&);"]
            lines += ["namespace {", "void Gather(TArray<FLhatNativeType>& Types) {", *[f"    Gather_{group}_{k}(Types);" for k in units], "}",
                      "bool Register(LhatProgram* P, FLhatBindings& B) {", *[f"    if (!Register_{group}_{k}(P, B)) return false;" for k in units], "    return true;", "}",
                      f'const FLhatNativeBindingProvider Provider{{TEXT("{group}"), Gather, Register}};', "}",
                      f"class F{group}Module final : public IModuleInterface {{", "public:",
                      "    virtual void StartupModule() override { LhatUEBindings::AddProvider(&Provider); }",
                      "    virtual void ShutdownModule() override { LhatUEBindings::RemoveProvider(&Provider); }",
                      "    virtual bool SupportsDynamicReloading() override { return false; }", "};",
                      f"IMPLEMENT_MODULE(F{group}Module, {group})", ""]
            files[f"Source/{group}/Private/{group}.cpp"] = "\n".join(lines)
            dependency_list = ", ".join(quote(d) for d in sorted(dependencies))
            editor_dependency_list = ", ".join(quote(d) for d in sorted(editor_dependencies - dependencies))
            conditional_dependencies = f'        if (Target.bBuildEditor) PrivateDependencyModuleNames.AddRange(new string[] {{ {editor_dependency_list} }});\n' if editor_dependency_list else ""
            files[f"Source/{group}/{group}.Build.cs"] = f'''// Generated by GenerateNativeBindings.py; do not edit.
using UnrealBuildTool;
public class {group} : ModuleRules
{{
    public {group}(ReadOnlyTargetRules Target) : base(Target)
    {{
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        PrivateDependencyModuleNames.AddRange(new string[] {{ {dependency_list} }});
{conditional_dependencies}\
    }}
}}
'''
            report["groups"][group] = {"types": sum(len(v["types"]) for v in modules.values()), "functions": len(self.functions[group]),
                                       "translation_units": len(units), "dependencies": sorted(dependencies)}
            report["groups"][group]["editor_dependencies"] = sorted(editor_dependencies - dependencies)
        report["generated_functions"] = [{"path": e["function"]["path"], "signature": e["signature"], "group": g}
                                         for g in (RUNTIME, EDITOR) for e in self.functions[g]]
        report["skipped_reasons"] = dict(Counter(r["reason"] for r in self.rejected).most_common())
        files["Saved/LhatNativeBindings/report.json"] = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        return files, report


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".lhat-", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def write_owned(project, files, install, *, manifest_relative=None, allowed_roots=None):
    manifest_path = project.parent / (manifest_relative or f"Source/{RUNTIME}/LhatGeneratedFiles.json")
    legacy_manifest = project.parent / "Saved/LhatNativeBindings/generated-files.json"
    previous_path = manifest_path if manifest_relative or manifest_path.exists() else legacy_manifest
    previous = json.loads(previous_path.read_text(encoding="utf-8")) if previous_path.exists() else {"files": {}}
    pending_path = manifest_path.with_suffix(".pending.json")
    pending = json.loads(pending_path.read_text(encoding="utf-8")) if pending_path.exists() else {"files": {}}
    roots = allowed_roots or [f"Source/{RUNTIME}/", f"Source/{EDITOR}/", "Saved/LhatNativeBindings/"]
    prior_paths = previous["files"].keys() | pending["files"].keys()
    for relative in set(files) | prior_paths:
        target = (project.parent / relative).resolve()
        owned_root = next((project.parent / root for root in roots if relative.startswith(root)), None)
        if owned_root is None or not target.is_relative_to(project.parent.resolve()) or not target.is_relative_to(owned_root.resolve()):
            raise ValueError(f"Unsafe generated path: {relative}")
        if target.exists():
            allowed = {previous["files"].get(relative), pending["files"].get(relative)}
            # A stopped/locked generation may already have written this exact output.
            if relative in files:
                allowed.add(digest(files[relative].encode("utf-8")))
            if digest(target.read_bytes()) not in allowed:
                raise ValueError(f"Refusing to overwrite unowned/edited file: {target}")
    document = json.loads(project.read_text(encoding="utf-8-sig"))
    if install:
        for name, kind in ((RUNTIME, "Runtime"), (EDITOR, "Editor")):
            existing = next((m for m in document.get("Modules", []) if m["Name"] == name), None)
            definition = {"Name": name, "Type": kind, "LoadingPhase": "None", "PlatformAllowList": ["Win64"]}
            if existing == dict(definition, LoadingPhase="Default") and previous["files"]:
                existing["LoadingPhase"] = "None"
            if existing and existing != definition:
                raise ValueError(f"Existing module configuration differs: {name}")
            if not existing:
                document.setdefault("Modules", []).append(definition)
    state = {"version": VERSION, "files": {p: digest(c.encode('utf-8')) for p, c in sorted(files.items())}}
    state_bytes = (json.dumps(state, indent=2) + "\n").encode("utf-8")
    atomic_write(pending_path, state_bytes)
    for relative, content in files.items():
        target = project.parent / relative
        data = content.encode("utf-8")
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != data:
            atomic_write(target, data)
    for relative in prior_paths - files.keys():
        (project.parent / relative).unlink(missing_ok=True)
    atomic_write(manifest_path, state_bytes)
    pending_path.unlink()
    if install:
        data = (json.dumps(document, ensure_ascii=False, indent="\t") + "\n").encode("utf-8")
        if project.read_bytes() != data:
            atomic_write(project, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True, help="Directory containing flat UHT module JSON files")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--context", type=Path, help="LhatNativeContext commandlet JSON (defaults to the snapshot's context.json)")
    parser.add_argument("--install", action="store_true", help="Add generated Runtime/Editor modules to .uproject")
    parser.add_argument("--bundled-manifest", type=Path, default=Path(__file__).resolve().parents[1] / "Bindings/BundledEngineApi.generated.json")
    args = parser.parse_args()
    context_path = args.context or args.snapshot.resolve().parents[1] / "context.json"
    if not context_path.is_file():
        parser.error("Missing enabled-plugin context. Use GenerateNativeBindings.ps1 or supply --context from -run=LhatNativeContext.")
    context = json.loads(context_path.read_text(encoding="utf-8-sig"))
    if Path(context["project"]).resolve() != args.project.resolve():
        parser.error("Plugin context belongs to a different project")
    generator = Generator(args.snapshot.resolve(), context)
    generator.collect()
    if args.bundled_manifest.is_file():
        generator.exclude_bundled(json.loads(args.bundled_manifest.read_text(encoding="utf-8")))
    files, report = generator.emit()
    write_owned(args.project.resolve(strict=True), files, args.install)
    counts = {g: {k: v for k, v in data.items() if k in {"types", "functions", "translation_units"}} for g, data in report["groups"].items()}
    print(json.dumps({"groups": counts, "skipped": len(report["skipped"]), "report": "Saved/LhatNativeBindings/report.json"}, indent=2))


if __name__ == "__main__":
    main()
