"""Generate UE-decorated C headers and DLL exports from public declarations.

Pure static-inline value helpers remain local. dllimport/dllexport decoration
keeps every stateful API call in the one Lhat DLL, including its allocator.
"""
import argparse
from pathlib import Path
import re


def functions(text):
    # C-mode preprocessing has removed comments, conditional APIs and extern-C
    # blocks. Only file-scope declarations ending in ';' can contribute entries.
    text = re.sub(r'^\s*#.*$', '', text, flags=re.MULTILINE)
    names, statement, depth = set(), [], 0
    for char in text:
        if char == '{':
            depth += 1
            statement = []
        elif char == '}':
            depth -= 1
            statement = []
        elif depth == 0:
            statement.append(char)
            if char == ';':
                declaration = ''.join(statement)
                match = re.search(r'\b(lhat_[A-Za-z0-9_]+)\s*\(', declaration)
                if match and not re.search(r'\b(static|typedef)\b', declaration[:match.start()]):
                    names.add(match[1])
                statement = []
    return sorted(names)


def decorate(text, names):
    # Match declarations, never calls in comments or inline helper bodies. Put
    # __declspec before the return type (not after a pointer's '*'). Fail closed
    # below if a future public declaration uses an unsupported return spelling.
    code = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text, flags=re.DOTALL)
    pattern = re.compile(r'(?m)^[ \t]*(?:(?:const|struct|unsigned|signed)\s+)*'
                         r'(?:Lhat\w+|void|bool|char|short|int|long|float|double|size_t|u?int\d+_t)'
                         r'[ \t*]+(' + '|'.join(names) + r')\s*(?=\()')
    matches = list(pattern.finditer(code))
    for match in reversed(matches):
        text = text[:match.start()] + 'LHAT_API ' + text[match.start():]
    return text, {match[1] for match in matches}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preprocessed', type=Path, required=True)
    parser.add_argument('--symbols', type=Path, required=True, help='dumpbin /linkermember:2 output for the built core/port libraries')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    names = functions(args.preprocessed.read_text(encoding='utf-8-sig'))
    if not {'lhat_program_new', 'lhat_register_member', 'lhat_set_allocator', 'lhat_machine_new'} <= set(names):
        raise ValueError('Incomplete public API preprocessing input')
    # Some public debugger declarations remain visible when that optional feature
    # is disabled. Export only definitions actually present in this core build.
    definitions = set(re.findall(r'^\s*[0-9A-Fa-f]+\s+(lhat_\w+)\s*$', args.symbols.read_text(encoding='utf-8-sig'), re.MULTILINE))
    exports = sorted(set(names) & definitions)
    if not {'lhat_program_new', 'lhat_register_member', 'lhat_set_allocator', 'lhat_machine_new'} <= set(exports):
        raise ValueError('Incomplete built core API symbol input')
    root = Path(__file__).resolve().parents[1] / 'LhatCore/include'
    outputs = {}
    decorated = set()
    for source in root.rglob('*.h'):
        text = source.read_text(encoding='utf-8')
        text, found = decorate(text, names)
        decorated.update(found)
        outputs[args.output / 'include' / source.relative_to(root)] = text
    if set(names) - decorated:
        raise ValueError(f'Unsupported public declarations: {sorted(set(names) - decorated)}')
    outputs[args.output / 'LhatCoreExports.inl'] = '// Generated C exports from the pinned core.\n' + ''.join(f'#pragma comment(linker, "/EXPORT:{name}")\n' for name in exports)
    for output, text in outputs.items():
        if args.check:
            if not output.exists() or output.read_text(encoding='utf-8') != text:
                raise ValueError('UE core API headers are stale; run BuildLhat.ps1')
        elif not output.exists() or output.read_text(encoding='utf-8') != text:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(text, encoding='utf-8', newline='\n')
    print(f'{len(exports)} public core API functions share one Lhat DLL instance ({len(names) - len(exports)} optional declarations not built).')


if __name__ == '__main__':
    main()
