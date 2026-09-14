"""Fast generation/ownership regression tests, without an Unreal installation."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from GenerateNativeBindings import Generator, RUNTIME, EDITOR, write_owned
import GenerateNativeBindings as generator_module


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lhat-native-generator-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.snapshot = self.root / "snapshot"
        self.snapshot.mkdir()
        self.header = self.root / "Modules/Fixture/Public/Fixture.h"
        self.header.parent.mkdir(parents=True)
        self.header.write_text("#pragma once\n", encoding="utf-8")
        self.header.parent.parent.joinpath("Fixture.Build.cs").write_text("// test\n", encoding="utf-8")
        self.base = {"kind": "class", "name": "Object", "cpp_name": "UObject", "path": "/Script/CoreUObject.Object",
                     "base_path": None, "flags": "Native, RequiredAPI", "define_scope": "None", "metadata": {}, "functions": [],
                     "header": {"file": str(self.header), "include": "Fixture.h", "relative": "Public/Fixture.h"}}
        self.owner = dict(self.base, name="Fixture", cpp_name="UFixture", path="/Script/Fixture.Fixture",
                          base_path=self.base["path"], metadata={"BlueprintType": "true"}, functions=[])
        self.fn = {"name": "Add", "path": "/Script/Fixture.Fixture:Add", "flags": "Native, Static, Public, BlueprintCallable, BlueprintPure",
                   "export_flags": "CppStatic", "define_scope": "None", "metadata": {}, "parameters": [
                       {"name": "A", "cpp_type": "int32", "property_class": "IntProperty", "flags": "Parm", "references": []},
                       {"name": "ReturnValue", "cpp_type": "int32", "property_class": "IntProperty", "flags": "Parm, ReturnParm, OutParm", "references": []}]}
        self.project = self.root / "Fixture.uproject"
        self.project.write_text('{"FileVersion":3,"Modules":[]}', encoding="utf-8")

    def generate(self, function=None, module_type="GameRuntime", context=None):
        self.owner["functions"] = [self.fn if function is None else function]
        for name, kind, types in (("CoreUObject", "EngineRuntime", [self.base]), ("Fixture", module_type, [self.owner])):
            (self.snapshot / (name + ".json")).write_text(json.dumps({"module": name, "module_type": kind, "types": types}), encoding="utf-8")
        generator = Generator(self.snapshot, context)
        generator.collect()
        return generator.emit()

    def test_direct_calls_without_exposure_metadata(self):
        files, report = self.generate()
        cpp = files[f"Source/{RUNTIME}/Private/Fixture_0.cpp"]
        self.assertIn("UFixture::Add(Arg0)", cpp)
        self.assertNotIn("ProcessEvent", cpp)
        self.assertIn('"f^number^ -> number^;"', cpp)
        self.assertEqual(report["groups"][RUNTIME]["functions"], 1)
        self.assertEqual(report["groups"][EDITOR]["functions"], 0)

    def test_editor_partition(self):
        _, report = self.generate(module_type="GameEditor")
        self.assertEqual(report["groups"][RUNTIME]["functions"], 0)
        self.assertEqual(report["groups"][EDITOR]["functions"], 1)
        self.assertNotIn("Fixture", report["groups"][RUNTIME]["dependencies"])

    def test_with_editor_method_of_runtime_class_uses_editor_module(self):
        fn = copy.deepcopy(self.fn)
        fn["define_scope"] = "Editor"
        fn["parameters"][0]["define_scope"] = "Editor"
        _, report = self.generate(fn)
        self.assertEqual(report["groups"][RUNTIME]["functions"], 0)
        self.assertEqual(report["groups"][EDITOR]["functions"], 1)
        self.assertEqual(report["groups"][RUNTIME]["types"], 1)

    def test_build_dependency_does_not_enable_a_plugin(self):
        _, report = self.generate(context={"modules": {"Fixture": {"plugin": "OptionalFeature", "enabled": False}}})
        self.assertEqual(report["groups"][RUNTIME]["functions"], 0)
        self.assertEqual(report["skipped"][0]["reason"], "plugin_disabled:OptionalFeature")
        self.assertNotIn("Fixture", report["groups"][RUNTIME]["dependencies"])

    def test_explicitly_loaded_plugin_is_not_forced_into_startup(self):
        _, report = self.generate(context={"modules": {"Fixture": {"plugin": "GameFeature", "enabled": True, "explicitly_loaded": True}}})
        self.assertEqual(report["skipped"][0]["reason"], "plugin_explicitly_loaded:GameFeature")

    def test_out_reference_is_reported(self):
        fn = copy.deepcopy(self.fn)
        fn["parameters"][0]["flags"] += ", OutParm"
        _, report = self.generate(fn)
        self.assertEqual(report["skipped"][0]["reason"], "out_or_mutable_reference")

    def test_minimal_api_does_not_export_ordinary_functions(self):
        self.owner["flags"] = "Native, MinimalAPI"
        _, report = self.generate()
        self.assertEqual(report["skipped"][0]["reason"], "function_not_exported")

    def test_protected_function_is_reported(self):
        fn = dict(self.fn, flags=self.fn["flags"].replace("Public", "Protected"))
        _, report = self.generate(fn)
        self.assertEqual(report["skipped"][0]["reason"], "nonpublic_function")

    def test_private_header_is_reported(self):
        self.owner["header"] = dict(self.owner["header"], relative="Private/Fixture.h")
        _, report = self.generate()
        self.assertEqual(report["skipped"][0]["reason"], "nonpublic_header")

    def test_public_header_requiring_private_header_is_reported(self):
        private = self.header.parent.parent / "Private/Secret.h"
        private.parent.mkdir()
        private.write_text("// internal API\n", encoding="utf-8")
        self.header.write_text('#include "Secret.h"\n', encoding="utf-8")
        _, report = self.generate()
        self.assertEqual(report["skipped"][0]["reason"], "header_requires_nonpublic_include:Secret.h")

    def test_public_sibling_include_adds_module_dependency(self):
        sibling = self.header.parent.parent.parent / "Sibling"
        (sibling / "Public").mkdir(parents=True)
        (sibling / "Sibling.Build.cs").write_text("// module\n", encoding="utf-8")
        (sibling / "Public/Other.h").write_text("// public API\n", encoding="utf-8")
        self.header.write_text('#include "Other.h"\n', encoding="utf-8")
        _, report = self.generate()
        self.assertIn("Sibling", report["groups"][RUNTIME]["dependencies"])
        _, report = self.generate(context={"modules": {"Sibling": {"plugin": "Disabled", "enabled": False}}})
        self.assertEqual(report["skipped"][0]["reason"], "header_requires_plugin_disabled:Disabled")

    def test_native_events_are_not_accidentally_devirtualized(self):
        fn = dict(self.fn, flags=self.fn["flags"] + ", Event")
        _, report = self.generate(fn)
        self.assertEqual(report["skipped"][0]["reason"], "event_rpc_or_delegate")

    def test_determinism_and_no_rewrite(self):
        files, _ = self.generate()
        write_owned(self.project, files, True)
        cpp = self.root / f"Source/{RUNTIME}/Private/Fixture_0.cpp"
        before = cpp.stat().st_mtime_ns
        files2, _ = self.generate()
        self.assertEqual(files, files2)
        write_owned(self.project, files2, True)
        self.assertEqual(before, cpp.stat().st_mtime_ns)
        self.assertEqual(len(json.loads(self.project.read_text())["Modules"]), 2)

    def test_hand_edits_are_preserved(self):
        files, _ = self.generate()
        write_owned(self.project, files, True)
        cpp = self.root / f"Source/{RUNTIME}/Private/Fixture_0.cpp"
        cpp.write_text("// user edit", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "unowned/edited"):
            write_owned(self.project, files, True)
        self.assertEqual(cpp.read_text(), "// user edit")

    def test_interrupted_generation_recovers_with_new_output(self):
        files, _ = self.generate()
        write_owned(self.project, files, True)
        first = f"Source/{RUNTIME}/Private/Fixture_0.cpp"
        second = f"Source/{EDITOR}/Private/{EDITOR}.cpp"
        changed = dict(files, **{first: files[first] + "// first revision\n", second: files[second] + "// first revision\n"})
        real_write = generator_module.atomic_write

        def interrupted(path, data):
            if path == self.root / second:
                raise PermissionError("simulated compiler file lock")
            real_write(path, data)

        with patch.object(generator_module, "atomic_write", interrupted):
            with self.assertRaises(PermissionError):
                write_owned(self.project, changed, True)
        changed[first] += "// next revision\n"
        write_owned(self.project, changed, True)
        self.assertEqual((self.root / first).read_text(), changed[first])
        self.assertFalse((self.root / f"Source/{RUNTIME}/LhatGeneratedFiles.pending.json").exists())

    def test_stale_generated_source_is_removed_without_touching_user_files(self):
        files, _ = self.generate()
        extra = f"Source/{RUNTIME}/Private/Retired.cpp"
        write_owned(self.project, dict(files, **{extra: "// generated fixture\n"}), True)
        user = self.root / f"Source/{RUNTIME}/Private/User.cpp"
        user.write_text("// user\n", encoding="utf-8")
        write_owned(self.project, files, True)
        self.assertFalse((self.root / extra).exists())
        self.assertEqual(user.read_text(), "// user\n")

    def test_manifest_cannot_delete_outside_generated_roots(self):
        files, _ = self.generate()
        write_owned(self.project, files, True)
        manifest = self.root / f"Source/{RUNTIME}/LhatGeneratedFiles.json"
        data = json.loads(manifest.read_text())
        data["files"]["Source/Unrelated/User.cpp"] = "fake"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unsafe generated path"):
            write_owned(self.project, files, True)


if __name__ == "__main__":
    unittest.main()
