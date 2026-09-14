"""Checks the UE-only import/export header generation without building UE."""
import unittest

from GenerateCoreApi import decorate, functions


class CoreApiTests(unittest.TestCase):
    def test_only_public_declarations(self):
        source = '''
#line 1 "lhat.h"
typedef void (*LhatHostFn)(void*);
void lhat_free(void* pointer);
const struct LhatProto *lhat_unit_proto(const LhatUnit* unit);
static inline bool lhat_is_nil(LhatValue v) { return v.kind == 0; }
typedef void lhat_not_a_function(void);
struct LhatValue { int kind; };
bool lhat_program_compile(LhatProgram* program);
'''
        self.assertEqual(functions(source), ['lhat_free', 'lhat_program_compile', 'lhat_unit_proto'])

    def test_pointer_return_decoration_precedes_type(self):
        text = 'const struct LhatProto *lhat_unit_proto(const LhatUnit* unit);\n'
        output, found = decorate(text, ['lhat_unit_proto'])
        self.assertEqual(output, 'LHAT_API ' + text)
        self.assertEqual(found, {'lhat_unit_proto'})

    def test_comments_and_inline_calls_are_not_decorated(self):
        text = '''// void lhat_free(void* pointer);
/* a multiline comment
void lhat_free(void* pointer);
*/
void lhat_free(void* pointer);
static inline void helper(void* p) { lhat_free(p); }
static inline bool helper2(void* p) {
    return lhat_program_compile(p);
}
'''
        output, found = decorate(text, ['lhat_free', 'lhat_program_compile'])
        self.assertEqual(output.count('LHAT_API'), 1)
        self.assertEqual(found, {'lhat_free'})
        self.assertIn('LHAT_API void lhat_free', output)
        self.assertIn('return lhat_program_compile(p)', output)

    def test_multiline_parameters(self):
        text = 'bool lhat_register_member(LhatProgram* program,\n    const char* module);\n'
        output, found = decorate(text, ['lhat_register_member'])
        self.assertEqual(output, 'LHAT_API ' + text)
        self.assertEqual(found, {'lhat_register_member'})


if __name__ == '__main__':
    unittest.main()
