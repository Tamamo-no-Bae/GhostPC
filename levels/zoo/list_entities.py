# Writes entity_list.txt: every Global.not template that is a character/unit (has an AI or a basedon chain reaching
# _Terran/_Protoss/_Zerg/_actor), with its resolved model, AI type, health/shield and the base it inherits from.
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import notdump as N

byname = {nm.lower(): off for nm, off in N.idx}
def chain(name, seen=None):
    """props of a template merged with its basedon parents (child overrides parent)"""
    seen = seen or set()
    off = byname.get(name.lower())
    if off is None or name.lower() in seen: return {}, []
    seen.add(name.lower())
    own = N.props(off)
    base = next((v for k, v in own if k == 'basedon'), None)
    merged, bases = chain(base, seen) if base else ({}, [])
    for k, v in own: merged[k] = v
    return merged, ([base] + bases if base else [])

rows = []
for nm, off in N.idx:
    p, bases = chain(nm)
    roots = [b.lower() for b in bases]
    side = next((s for s in ('_terran', '_protoss', '_zerg') if s in roots), None)
    if 'ACTOR_AI' not in p and not side: continue
    if nm.startswith('_'): continue
    rows.append((side[1:].capitalize() if side else '-', nm, p.get('model', ''), p.get('ACTOR_AI', ''),
                 p.get('ACTOR_HEALTH', ''), p.get('ACTOR_SHIELD', ''), bases[0] if bases else ''))
rows.sort(key=lambda r: (r[0], r[1].lower()))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entity_list.txt')
with open(out, 'w') as f:
    f.write('%-8s %-36s %-30s %-22s %6s %6s  %s\n' % ('side', 'template', 'model', 'AI', 'hp', 'shield', 'based on'))
    for r in rows: f.write('%-8s %-36s %-30s %-22s %6s %6s  %s\n' % r)
print(len(rows), 'templates ->', out)
