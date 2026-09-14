"""Offline bundle selection, target isolation and project de-duplication tests."""
import copy
import json
import unittest

import TestNativeGenerator
from GenerateNativeBindings import Generator, RUNTIME, EDITOR, write_owned
from GenerateBundledBindings import generate, DIRECTORY, MANIFEST


class BundledTests(unittest.TestCase):
    setUp = TestNativeGenerator.GeneratorTests.setUp

    def snapshot_files(self, editor=False):
        self.owner['path'] = '/Script/Engine.Fixture'
        self.fn['path'] = self.owner['path'] + ':Add'
        self.fn['define_scope'] = 'Editor' if editor else 'None'
        self.owner['functions'] = [self.fn]
        unrelated = dict(self.owner, name='Unrelated', cpp_name='UUnrelated', path='/Script/Engine.Unrelated', functions=[])
        for module, records in [('CoreUObject', [self.base]), ('Engine', [self.owner, unrelated])]:
            (self.snapshot / (module + '.json')).write_text(json.dumps({'module': module, 'module_type': 'EngineRuntime', 'types': records}), encoding='utf-8')
        return {'classes': [self.owner['path']]}

    def test_bundle_is_plugin_owned_and_direct(self):
        files, report = generate(self.snapshot, self.snapshot_files())
        cpp = '\n'.join(v for k, v in files.items() if k.endswith('.cpp'))
        self.assertIn('UFixture::Add(Arg0)', cpp)
        self.assertNotIn('ProcessEvent', cpp)
        self.assertNotIn('IMPLEMENT_MODULE', cpp)
        self.assertNotIn('UUnrelated', cpp)
        self.assertTrue(all(k.startswith(DIRECTORY + '/') or k == MANIFEST for k in files))
        self.assertEqual(report['groups'][RUNTIME]['functions'], 1)
        self.assertNotIn(str(self.root), files[MANIFEST])

    def test_editor_callbacks_are_compile_guarded(self):
        files, report = generate(self.snapshot, self.snapshot_files(editor=True))
        cpp = files[f'{DIRECTORY}/LhatBundledEditor_Engine_0.cpp']
        self.assertIn('#if WITH_EDITOR\n', cpp)
        self.assertTrue(cpp.endswith('#endif\n'))
        self.assertEqual(report['groups'][RUNTIME]['functions'], 0)
        self.assertEqual(report['groups'][EDITOR]['functions'], 1)

    def test_project_generation_subtracts_bundle_not_unrelated_api(self):
        profile = self.snapshot_files()
        _, manifest = generate(self.snapshot, profile)
        additional = copy.deepcopy(self.fn)
        additional['name'] = 'Extra'
        additional['path'] = self.owner['path'] + ':Extra'
        generator = Generator(self.snapshot)
        generator.types[self.owner['path']]['functions'].append(additional)
        generator.collect()
        generator.exclude_bundled(manifest)
        files, report = generator.emit()
        self.assertIn(self.fn['path'], report['provided_by_bundle'])
        self.assertEqual([e['path'] for e in report['generated_functions']], [additional['path']])
        self.assertNotIn('UFixture::Add(', files[f'Source/{RUNTIME}/Private/Engine_0.cpp'])

    def test_signature_drift_requires_bundle_regeneration(self):
        _, manifest = generate(self.snapshot, self.snapshot_files())
        manifest['generated_functions'][0]['signature'] = 'f^string^ -> string^;'
        generator = Generator(self.snapshot)
        generator.collect()
        with self.assertRaisesRegex(ValueError, 'Bundled API signature/target changed'):
            generator.exclude_bundled(manifest)

    def test_missing_profile_class_is_an_error(self):
        self.snapshot_files()
        with self.assertRaisesRegex(ValueError, 'Missing bundled class'):
            generate(self.snapshot, {'classes': ['/Script/Engine.DoesNotExist']})

    def test_bundle_writer_preserves_edits_and_is_deterministic(self):
        profile = self.snapshot_files()
        files, _ = generate(self.snapshot, profile)
        files2, _ = generate(self.snapshot, profile)
        self.assertEqual(files, files2)
        kwargs = dict(manifest_relative=f'{DIRECTORY}/LhatBundledFiles.json', allowed_roots=[DIRECTORY + '/', MANIFEST])
        write_owned(self.project, files, False, **kwargs)
        path = self.root / DIRECTORY / 'LhatBundledBindings.cpp'
        timestamp = path.stat().st_mtime_ns
        write_owned(self.project, files, False, **kwargs)
        self.assertEqual(path.stat().st_mtime_ns, timestamp)
        path.write_text('// user change', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'unowned/edited'):
            write_owned(self.project, files, False, **kwargs)


if __name__ == '__main__':
    unittest.main()
