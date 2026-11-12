#!/usr/bin/env python3
"""Compile the HLSL embedded in a C++ source with the system d3dcompiler_47.dll, the DLL EDVR compiles it with at run
time, so a syntax error fails the build and not a flight.

Usage: python tools/hlsl_embedded_check.py [src\\d3d11\\hull_layer.cpp ...]
       python tools/hlsl_embedded_check.py --self-test

Why (2026-11-08, docs/research-own-hull-layer-2026-11-08.md): the hull layer's v2 shaders are compiled at run time.
A cbuffer named `Pass` (an HLSL keyword) failed in the game, and a whole test flight logged `state=no-shader`.

A source opts in with one comment line per shader:
    // hlsl-check: <constant name> <entry point> <target>
For example: `// hlsl-check: kSeedHlsl hull_layer_seed_cs cs_5_0`. The constant is a `constexpr char kName[] = ...;`
made of adjacent string literals and #define macros that are themselves string literals. With no files given, the
script checks every source under src\\ that has such a line. Exit 0 when all compile, 1 when any fails or a named
constant is missing, 2 when the compiler cannot be loaded (on a non-Windows host it reports that and exits 0, since
the check needs Windows' compiler).
"""
import argparse
import ctypes
import glob
import os
import re
import sys

DIRECTIVE = re.compile(r'//\s*hlsl-check:\s*(\w+)\s+(\w+)\s+(\w+_\d_\d)')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def literals(chunk):
    # C++ comment lines between the literals are not HLSL; a quoted phrase in one was read as code once (2026-11-08).
    chunk = re.sub(r'^\s*//.*$', '', chunk, flags=re.M)
    return ''.join(bytes(m.group(1), 'utf-8').decode('unicode_escape') for m in LITERAL.finditer(chunk))


def extract(text):
    """{constant: hlsl text}, macros expanded where a constant names them."""
    macros = {m.group(1): literals(m.group(2)) for m in re.finditer(r'#define (\w+)\s+((?:.*\\\n)*.*)', text)}
    out = {}
    for m in re.finditer(r'constexpr char (\w+)\[\] =(.*?);\n', text, re.S):
        body = m.group(2)
        pre = ''.join(macros[w] for w in re.findall(r'\b([A-Z][A-Z0-9_]+)\b', body) if w in macros)
        out[m.group(1)] = pre + literals(body)
    return out


def compiler():
    if os.name != 'nt':
        return None
    from ctypes import wintypes
    dll = ctypes.WinDLL('d3dcompiler_47.dll')
    f = dll.D3DCompile
    f.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                  ctypes.c_char_p, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p),
                  ctypes.POINTER(ctypes.c_void_p)]
    return f


def blob_text(p):
    if not p:
        return ''
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(p)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(p)
    text = ctypes.string_at(ptr, size).decode('latin1') if ptr else ''   # read before the Release frees it
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(p)   # Release
    return text


def compile_one(fn, name, src, entry, target):
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = fn(src.encode(), len(src), name.encode(), None, None, entry.encode(), target.encode(), 0, 0,
            ctypes.byref(code), ctypes.byref(err))
    msg = blob_text(err.value).strip()
    if code.value:
        blob_text(code.value)
    return hr == 0, msg


def check_text(fn, text, label):
    bad = 0
    shaders = extract(text)
    for const, entry, target in DIRECTIVE.findall(text):
        src = shaders.get(const)
        if src is None:
            print(f'{label}: {const}: not found')
            bad += 1
            continue
        ok, msg = compile_one(fn, const, src, entry, target)
        print(f'{label}: {const} {entry} {target}: {"ok" if ok else "FAILED"}' + ('' if ok else f'\n    {msg[:400]}'))
        bad += 0 if ok else 1
    return bad


def self_test():
    fn = compiler()
    if fn is None:
        print('hlsl_embedded_check self-test: not Windows, skipped')
        return 0
    good = ('// hlsl-check: kGood good_cs cs_5_0\n#define CB "cbuffer GoodCb : register(b0) { uint n; };\\n"\n'
            'constexpr char kGood[] = CB\n    "RWByteAddressBuffer o : register(u0);\\n"\n'
            '    // a C++ comment with a "quoted phrase" between the literals is not HLSL\n'
            '    "[numthreads(1, 1, 1)] void good_cs() { o.Store(0, n); }\\n";\n')
    bad = good.replace('GoodCb', 'Pass').replace('kGood', 'kBad').replace('good_cs', 'bad_cs')
    missing = '// hlsl-check: kNone none_cs cs_5_0\n'
    assert extract(good)['kGood'].startswith('cbuffer GoodCb'), extract(good)
    assert check_text(fn, good, 'self-test good') == 0
    assert check_text(fn, bad, 'self-test bad (a cbuffer named Pass)') == 1
    assert check_text(fn, missing, 'self-test missing') == 1
    print('hlsl_embedded_check self-test: ok')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    try:
        fn = compiler()
    except OSError as e:
        print(f'hlsl_embedded_check: d3dcompiler_47.dll not loadable: {e}')
        return 2
    if fn is None:
        print('hlsl_embedded_check: not Windows, skipped')
        return 0
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = a.files or [p for p in glob.glob(os.path.join(root, 'src', '**', '*.cpp'), recursive=True)
                        if DIRECTIVE.search(open(p, encoding='utf-8', errors='replace').read())]
    bad = sum(check_text(fn, open(p, encoding='utf-8').read(), os.path.relpath(p, root)) for p in files)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
