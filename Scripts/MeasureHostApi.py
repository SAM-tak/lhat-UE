"""Measure real UE declarations in the lhat-host JSON envelope, without binding them.

Structs, delegates and containers get nominal placeholder types for this experiment.
This preserves every selected signature, but is NOT an executable ABI or a finished
type mapping. Raw UHT/Blueprint records retain the original types and metadata.
"""

import argparse
import collections
import gzip
import hashlib
import json
import pathlib
import re
import statistics
import time


def flags(record):
    return set(record.get("flags", "").split(", "))


def meta_true(record, key):
    value = record.get("metadata", {}).get(key)
    return value is not None and str(value).lower() not in ("false", "0")


def identifier(name):
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return name if name and not name[0].isdigit() else "_" + name


def blueprint_function(record):
    return bool(flags(record) & {"BlueprintCallable", "BlueprintPure", "BlueprintEvent"}) and "Delegate" not in flags(record) and not meta_true(record, "BlueprintInternalUseOnly")


def blueprint_property(record):
    return bool(flags(record) & {"BlueprintVisible", "BlueprintAssignable", "BlueprintCallable"})


def dump_bytes(value, **kwargs):
    return json.dumps(value, ensure_ascii=False, **kwargs).encode("utf-8")


def host_bytes(document):
    """One declaration per line, like lhat_program_dump_host_api (not deeply indented)."""
    lines = ['{', '  "strict": true,']
    keys = ("types", "functions", "annotations", "bindings")
    for index, key in enumerate(keys):
        lines.append('  "' + key + '": [')
        entries = document[key]
        lines.extend("    " + json.dumps(entry, ensure_ascii=False) + ("," if i + 1 < len(entries) else "") for i, entry in enumerate(entries))
        lines.append("  ]" + ("," if index + 1 < len(keys) else ""))
    return ("\n".join(lines + ["}", ""])).encode("utf-8")


class Projection:
    def __init__(self, source, universe):
        self.source = source
        self.universe = universe
        self.types = {}
        self.names = {}
        self.used_names = set()
        self.functions = []
        self.declared_functions = []
        self.documented_functions = []
        self.function_count = 0
        self.property_count = 0
        self.event_count = 0
        self.missing_references = set()
        self.opaque = {}
        self.module_counts = collections.Counter()

    def address(self, path):
        if path in self.names:
            return self.names[path]
        owner, _, name = path.rpartition(".")
        if owner.startswith("/Script/"):
            module = "ue." + identifier(owner.removeprefix("/Script/"))
        else:
            module = "bp." + ".".join(identifier(part) for part in owner.rsplit("/", 1)[0].strip("/").split("/"))
        result = module, identifier(name)
        # Preserve identity even if sanitizing paths produces a spelling collision.
        if result in self.used_names:
            result = module, result[1] + "_" + hashlib.sha256(path.encode()).hexdigest()[:12]
        self.names[path] = result
        self.used_names.add(result)
        return result

    def ensure_type(self, path):
        if path in self.types:
            module, name = self.address(path)
            return module + "." + name
        original = self.universe.get(path)
        module, name = self.address(path)
        if original is None:
            self.missing_references.add(path)
        entry = {"kind": "hostdata", "module": module, "name": name}
        if original and original["kind"] == "enum" and original.get("values"):
            members = [identifier(item["name"].split("::")[-1]) for item in original["values"]]
            if len(set(members)) == len(members):
                entry = {"kind": "enum", "module": module, "name": name, "members": members,
                         "values": [item["value"] for item in original["values"]]}
        # Add bases first, as the Lhat host-config loader requires.
        if original and original.get("base_path"):
            base = original["base_path"]
            self.ensure_type(base)
            entry.update(zip(("base_module", "base_name"), self.address(base)))
        self.types[path] = entry
        return module + "." + name

    def type_of(self, prop):
        kind = prop["property_class"]
        refs = prop.get("references", [])
        for path in refs:
            self.ensure_type(path)
        if not prop.get("array_dimension"):
            if kind in {"IntProperty", "Int8Property", "Int16Property", "Int64Property", "UInt16Property", "UInt32Property", "UInt64Property", "FloatProperty", "DoubleProperty", "ByteProperty"} and not refs:
                return "number^"
            if kind == "BoolProperty":
                return "bool^"
            if kind in {"StrProperty", "Utf8StrProperty", "AnsiStrProperty"}:
                return "string^"
            if kind in {"ObjectProperty", "StructProperty", "EnumProperty", "ByteProperty", "InterfaceProperty"} and len(refs) == 1:
                return self.ensure_type(refs[0])
        spelling = prop["cpp_type"] + ("[" + prop["array_dimension"] + "]" if prop.get("array_dimension") else "")
        key = spelling + "|" + "|".join(refs)
        if key not in self.opaque:
            digest = hashlib.sha256(key.encode()).hexdigest()[:16]
            label = identifier(spelling)[:48] + "_" + digest
            path = "/Script/SurveyOpaque." + label
            self.opaque[key] = {"cpp_type": spelling, "references": refs, "path": path}
            self.types[path] = {"kind": "hostdata", "module": "ue.SurveyOpaque", "name": label}
            self.names[path] = "ue.SurveyOpaque", label
            self.used_names.add(self.names[path])
        module, name = self.address(self.opaque[key]["path"])
        return module + "." + name

    def function(self, owner, fn):
        inputs, outputs = [], []
        if "Static" not in flags(fn):
            inputs.append("self^")
        for prop in fn.get("parameters", []):
            tag = self.type_of(prop)
            pf = flags(prop)
            if "ReturnParm" in pf:
                outputs.insert(0, tag)
            else:
                if "OutParm" not in pf or pf & {"ConstParm", "ReferenceParm"}:
                    inputs.append(tag)
                if "OutParm" in pf and "ConstParm" not in pf:
                    outputs.append(tag)
        pure = bool(flags(fn) & {"BlueprintPure", "Const"})
        signature = ("f^" if pure else "p^") + ", ".join(inputs)
        if outputs:
            signature += " -> " + ", ".join(outputs)
        signature += ";"
        module, name = self.address(owner["path"])
        record = {"kind": "member", "module": module, "type": name, "name": identifier(fn["name"]), "signature": signature}
        self.functions.append(record)
        self.declared_functions.append(record)
        documented = dict(record)
        if fn.get("metadata", {}).get("ToolTip"):
            documented["documentation"] = fn["metadata"]["ToolTip"]
        documented["ue_parameters"] = [{"name": p["name"], "cpp_type": p["cpp_type"], "flags": p.get("flags", "")} for p in fn.get("parameters", [])]
        self.documented_functions.append(documented)
        self.function_count += 1
        self.event_count += "BlueprintEvent" in flags(fn)
        self.module_counts[owner["_module"]] += 1

    def property(self, owner, prop):
        tag = self.type_of(prop)
        module, name = self.address(owner["path"])
        modes = [("get_", "f^self^ -> " + tag + ";")]
        if "BlueprintReadOnly" not in flags(prop):
            modes.append(("set_", "p^self^, " + tag + ";"))
        # Measurement-only accessor names; the public naming policy is not decided.
        for prefix, signature in modes:
            record = {"kind": "member", "module": module, "type": name, "name": prefix + identifier(prop["name"]), "signature": signature}
            self.functions.append(record)
            documented = dict(record)
            if prop.get("metadata", {}).get("ToolTip"):
                documented["documentation"] = prop["metadata"]["ToolTip"]
            self.documented_functions.append(documented)
        self.property_count += 1

    def build(self, only_blueprint):
        def blueprint_type(record, visiting=None):
            visiting = set() if visiting is None else visiting
            if record["path"] in visiting:
                return False
            visiting.add(record["path"])
            metadata = record.get("metadata", {})
            if "BlueprintType" in metadata:
                return meta_true(record, "BlueprintType")
            if meta_true(record, "IsBlueprintBase") or record.get("asset"):
                return True
            parent = self.universe.get(record.get("base_path"))
            return bool(parent and blueprint_type(parent, visiting))

        for owner in sorted(self.source, key=lambda item: item["path"]):
            funcs = [fn for fn in owner.get("functions", []) if not only_blueprint or blueprint_function(fn)]
            props = [prop for prop in owner.get("properties", []) if not only_blueprint or blueprint_property(prop)]
            if funcs or props or not only_blueprint or blueprint_type(owner):
                self.ensure_type(owner["path"])
            for fn in funcs:
                self.function(owner, fn)
            for prop in props:
                self.property(owner, prop)
        return {"strict": True, "types": list(self.types.values()), "functions": self.functions, "annotations": [], "bindings": []}


def measure(directory, name, projection, document):
    output = directory / name
    output.mkdir(parents=True, exist_ok=False)
    plain = host_bytes(document)
    compact = dump_bytes(document, separators=(",", ":"))
    pretty = dump_bytes(document, indent=2)
    functions_only = dict(document, functions=projection.declared_functions)
    functions_only_bytes = host_bytes(functions_only)
    documented = dict(document)
    documented["functions"] = projection.documented_functions
    documented["types"] = [dict(item, **({"documentation": projection.universe[path]["metadata"]["ToolTip"]} if projection.universe.get(path, {}).get("metadata", {}).get("ToolTip") else {})) for path, item in projection.types.items()]
    docs = host_bytes(documented)
    (output / "lhat-host.json").write_bytes(plain)
    (output / "lhat-host.min.json").write_bytes(compact)
    (output / "lhat-host.documented.json").write_bytes(docs)
    (output / "lhat-host.functions-only.json").write_bytes(functions_only_bytes)
    (output / "opaque-types.json").write_bytes(dump_bytes(list(projection.opaque.values()), indent=2))
    # JSON parse only, in the Python implementation. NOT Lhat type registration/LSP latency.
    timings = []
    for _ in range(5):
        start = time.perf_counter()
        parsed = json.loads(plain)
        timings.append((time.perf_counter() - start) * 1000)
        assert len(parsed["functions"]) == len(document["functions"])
        del parsed
    kinds = collections.Counter(projection.universe[path]["kind"] for path in projection.types if path in projection.universe)
    return {"name": name, "uclasses": kinds["class"], "ustructs": kinds["struct"], "uenums": kinds["enum"],
            "ufunctions": projection.function_count, "blueprint_events_in_functions": projection.event_count,
            "uproperties": projection.property_count, "generated_property_accessors": len(projection.functions) - projection.function_count,
            "all_type_entries": len(document["types"]), "opaque_type_entries": len(projection.opaque),
            "host_json_bytes": len(plain), "minified_bytes": len(compact), "deep_indent_bytes": len(pretty),
            "functions_only_bytes_with_same_type_table": len(functions_only_bytes),
            "with_documentation_and_parameter_names_bytes": len(docs), "gzip_minified_bytes": len(gzip.compress(compact, mtime=0)),
            "python_json_parse_median_ms": round(statistics.median(timings), 2),
            "missing_referenced_declarations": sorted(projection.missing_references),
            "top_function_modules": projection.module_counts.most_common(12)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=pathlib.Path)
    parser.add_argument("--blueprints", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    modules = [json.loads(path.read_text(encoding="utf-8-sig")) for path in sorted((args.snapshot / "UHT").rglob("*.json"))]
    manifest = json.loads((args.snapshot / "survey.uhtmanifest").read_text(encoding="utf-8-sig"))
    assert len(modules) == len(manifest["Modules"]), "Incomplete UHT export"
    universe = {}
    for module in modules:
        for entry in module["types"]:
            assert entry["path"] not in universe, "Duplicate type: " + entry["path"]
            entry["_module"] = module["module"]
            entry["_runtime_module"] = module["module_type"] in ("EngineRuntime", "GameRuntime")
            universe[entry["path"]] = entry
    native = list(universe.values())
    blueprints = json.loads(args.blueprints.read_text(encoding="utf-8-sig"))
    for entry in blueprints["types"]:
        assert entry["path"] not in universe, "Duplicate Blueprint type"
        entry["_module"] = "Blueprint:" + entry["asset"].split("/")[1]
        entry["_runtime_module"] = not entry.get("editor_only_asset", False)
        universe[entry["path"]] = entry
    args.output.mkdir(parents=True, exist_ok=False)
    summary = {"project": "FirstPersonTemplate", "native_modules": len(modules),
               "native_module_kinds": dict(collections.Counter(m["module_type"] for m in modules)),
               "native_types": dict(collections.Counter(t["kind"] for t in native)),
               "native_ufunctions": sum(len(t.get("functions", [])) for t in native),
               "blueprint_assets": blueprints["asset_count"], "blueprint_classes": len(blueprints["types"]),
               "blueprint_roots": dict(collections.Counter(t["asset"].split("/")[1] for t in blueprints["types"])),
               "blueprint_load_failures": blueprints["load_failures"], "blueprints_without_generated_class": blueprints["without_generated_class"],
               "measurements": []}
    for name, runtime, only_bp in [("runtime-blueprint", True, True), ("editor-blueprint", False, True), ("editor-all-reflected", False, False)]:
        source = [entry for entry in universe.values() if not runtime or entry["_runtime_module"]]
        projection = Projection(source, universe)
        document = projection.build(only_bp)
        summary["measurements"].append(measure(args.output, name, projection, document))
    (args.output / "summary.json").write_bytes(dump_bytes(summary, indent=2))
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
