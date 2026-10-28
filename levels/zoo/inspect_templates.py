# Prints, for each template: resolved model (through basedon), AI, type, health, model size and min z, and whether the
# model exists as a loose file or inside an .nmb bundle. Usage: py inspect_templates.py name [name...]
import os, sys, struct, glob
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import notdump as N
MODELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'game', '3D', 'Models')
loose = {f.lower(): os.path.join(MODELS, f) for f in os.listdir(MODELS)}
bundles = [open(f, 'rb').read() for f in glob.glob(os.path.join(MODELS, '*.nmb'))]

def resolve(name):
    merged, seen, n = {}, set(), name
    chain = []
    while n and n.lower() not in seen:
        seen.add(n.lower()); off = N.find(n)
        if off is None: chain.append('MISSING:' + n); break
        p = N.props(off); chain.append(n)
        for k, v in p:
            merged.setdefault(k, v)
        n = dict(p).get('basedon')
    return merged, chain

for name in sys.argv[1:]:
    p, chain = resolve(name)
    model = p.get('model', '')
    size = ''
    f = loose.get(model.lower())
    if f:
        b = open(f, 'rb').read(0x5c)
        mn = struct.unpack_from('<3f', b, 12); mx = struct.unpack_from('<3f', b, 24)
        size = 'size %s minz %.2f' % ([round(mx[i] - mn[i], 1) for i in range(3)], mn[2])
        where = 'loose'
    else:
        where = 'in-bundle' if model and any(model.lower().encode() in d.lower() for d in bundles) else 'MISSING'
    print('%-22s model %-26s %-9s AI %-22s type %-3s hp %-5s %s  chain %s' % (
        name, model, where, p.get('ACTOR_AI', '-'), p.get('type', '?'), p.get('ACTOR_HEALTH', '-'), size, '>'.join(chain[:4])))
