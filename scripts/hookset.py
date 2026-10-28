# Compute the set of XDK functions that must be replaced: those in replaced libs reachable
# directly from kept code (game, havok, CRT, bink, d3dx, xgraphics). Also kernel imports used by kept code.
import sys,os,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from callgraph import load
funcs,refs,obj=load()
REPL=set('d3d8 dsound xacteng xapilib xnet xboxkrnl'.split())
def lib(n): o=obj.get(n,''); return o.split(':')[0] if ':' in o else ''
kept=[n for n in funcs if lib(n) not in REPL]
hooks=collections.defaultdict(set); kern=collections.defaultdict(set)
for n in kept:
    for t in refs.get(n,()):
        if t.startswith('__imp_'): kern[t].add(n)
        elif t in funcs and lib(t) in REPL: hooks[t].add(n)
        elif lib(t) in REPL and not t.startswith('__imp_'): hooks[t].add(n)  # data symbol in replaced lib
by=collections.defaultdict(list)
for t,s in hooks.items(): by[lib(t)].append((t,len(s),'F' if t in funcs else 'D', sorted(set(lib(x) or 'game' for x in s))))
out=open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','re','hookset.txt'),'w')
for l in sorted(by):
    out.write('== %s %d\n'%(l,len(by[l])))
    for t,c,k,ls in sorted(by[l]): out.write('  %s %s %d %s\n'%(k,t,c,','.join(ls)))
out.write('== kernel imports used by kept code\n')
for t,s in sorted(kern.items()): out.write('  %s %s\n'%(t,sorted(s)[:6]))
out.close()
for l in sorted(by): print(l,len(by[l]),sum(1 for x in by[l] if x[2]=='F'))
print('kernel',len(kern))
