#!/usr/bin/env python3
"""gen_modnames.py OUT.h : name tables for host/modtest.c (symbolic parameter / destination names from the firmware's own headers)."""
import re, sys
H = __file__.rsplit('/tools/', 1)[0]
def enum_names(path, pre):
    s = open(path).read()
    out = []
    for m in re.finditer(r'enum\s*\{(.*?)\};', s, re.S):
        for n in re.findall(r'\b(' + pre + r'[A-Z0-9_]+)\b', m.group(1)):
            if n not in out: out.append(n)
    return out
names = []
for p in ['P_']: names += enum_names(H + '/src/params.h', p)
ids = open(H + '/src/pext_ids.h').read()
names += [n for n in re.findall(r'\bP_[A-Z0-9_]+', ids) if n not in names]
names = [n for n in names if n != 'P_COUNT']
v = []
for p in ['MG_', 'PR_', 'DR_', 'ARP_']:
    v += [n for n in enum_names(H + '/src/voices.h', p) if not n.endswith('COUNT') and n not in ('DR_PER',)]
dst = re.findall(r'\{[^}]*\},\s*/\* (\d+) (\w+) \*/', open(H + '/src/pext_gen.h').read())
o = open(sys.argv[1], 'w')
o.write('static const struct { const char *n; int v; } PVN[] = {\n' + ''.join(f'  {{"{n}", {n}}},\n' for n in names) + '  {0,0}};\n')
o.write('static const struct { const char *n; int v; } EPN[] = {\n' + ''.join(f'  {{"{n}", {n}}},\n' for n in v) + '  {0,0}};\n')
o.write('static const struct { const char *n; int v; } DSN[] = {\n' + ''.join(f'  {{"{n}", {i}}},\n' for i, n in dst) + '  {0,0}};\n')
