# Verify every xb_/kx_ replacement's compiler decoration matches the Ghost.map symbol it replaces
import sys,re
ours=set(); 
for l in open(sys.argv[1],errors='ignore'):
    for m in re.finditer(r'\s([_@])(xb|kx)_(\S+?)(@\d+)?\s',l): ours.add((m.group(1),m.group(2),m.group(3),m.group(4) or ''))
ghost=set()
for l in open(sys.argv[2],errors='ignore'):
    m=re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)',l)
    if m: ghost.add(m.group(1))
bad=0
for pre,kind,name,n in sorted(ours):
    want=pre+name+n
    if kind=='kx': want='__imp_'+want
    if want not in ghost:
        alts=[g for g in ghost if re.fullmatch(re.escape(pre+name)+r'(@\d+)?',g) or g=='__imp_'+pre+name+n]
        print('MISMATCH: ours %s%s_%s%s  ghost has %s'%(pre,kind,name,n,alts or 'nothing')); bad+=1
print('checkhooks: %d replacements, %d mismatches'%(len(ours),bad))
sys.exit(1 if bad else 0)
