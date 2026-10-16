"""Prints the Engine Fidelity tables (assets/render/fidelity/*.json) as Markdown for
docs/ENGINE_FIDELITY.md:  python tools/fidelity_table.py > /tmp/tables.md

Cells: "-" unavailable; "off" available, default off; otherwise the default, then the bounds
(max / min / allowed values). Era-owned features show the era scale ("era x3": the era's own value
times 3, a floor for error thresholds) or the cap ("era, max 0.25").
"""
import json
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DIR = os.path.join(ROOT, 'assets', 'render', 'fidelity')
LEVELS = ['off', 'low', 'medium', 'high', 'modern']


def fmt(v):
    return ('%g' % v) if isinstance(v, (int, float)) else str(v)


def cell(e):
    if not e:
        return '?'
    a = e.get('avail', 'on')
    if a == 'no':
        return '-'
    parts = []
    if 'scale' in e:
        parts.append('era x%s' % fmt(e['scale']))
    elif 'default' in e:
        parts.append(fmt(e['default']))
    elif a == 'off':
        parts.append('off')
    else:
        parts.append('era' if 'max' in e and 'default' not in e else 'on')
    if 'max' in e:
        parts.append('max ' + fmt(e['max']))
    if 'min' in e:
        parts.append('min ' + fmt(e['min']))
    if 'allowed' in e:
        parts.append('{' + ','.join(fmt(x) for x in e['allowed']) + '}')
    return ', '.join(parts)


def main():
    lv = {l: json.load(open(os.path.join(DIR, l + '.json'))) for l in LEVELS}
    eras = json.load(open(os.path.join(DIR, 'eras.json')))['eras']
    feats = list(lv['modern']['features'].keys())
    print('| feature | ' + ' | '.join(lv[l]['name'] for l in LEVELS) + ' |')
    print('|---|' + '---|' * len(LEVELS))
    for f in feats:
        print('| `%s` | ' % f + ' | '.join(cell(lv[l]['features'].get(f)) for l in LEVELS) + ' |')
    print()
    print('Budgets (1080p, mid-range GPU): ' + ', '.join('%s %g ms' % (lv[l]['name'], lv[l]['budget_ms']) for l in LEVELS) + '.')
    print()
    print('**Per-era table differences** (on top of the base table above):')
    print()
    print('| era | level | feature | entry |')
    print('|---|---|---|---|')
    for l in LEVELS:
        for e, fs in lv[l].get('eras', {}).items():
            for f, v in fs.items():
                print('| %s | %s | `%s` | %s%s |' % (e, lv[l]['name'], f, cell(v), (' (' + v['note'] + ')') if 'note' in v else ''))
    print()
    print('**Era capability matrix** (eras.json: what an era lacks is unavailable at every level):')
    print()
    print('| era | lacks | limits |')
    print('|---|---|---|')
    for e, d in eras.items():
        lim = '; '.join('`%s` %s' % (f, ', '.join(x for x in cell(dict(v, avail='on')).split(', ') if x not in ('on', 'era'))) for f, v in d.get('limits', {}).items())
        print('| %s | %s | %s |' % (e, ', '.join('`%s`' % x for x in d.get('lacks', [])) or '-', lim or '-'))


if __name__ == '__main__':
    main()
