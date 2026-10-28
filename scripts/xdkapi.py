# Which XDK-library functions are called/referenced from game (non-XDK) code
import sys,os,capstone,collections,re
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
g=Img(sys.argv[1],sys.argv[2])
XDK=set('d3d8 d3dx8 dsound xacteng xapilib xnet xboxkrnl LIBCMT binkxbox wmvdec xgraphics xbdm xonline xvoice dolby'.split())
def lib(o): return o.split(':')[0] if ':' in o else ''
libs=collections.Counter(lib(o) for a,n,o,f in g.syms if f)
print("libs:",libs.most_common())
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
calls=collections.defaultdict(set)
for a,e,n,o in g.funcs():
    if lib(o) in XDK: continue
    if e-a>0x20000: e=a+0x20000
    for ins in md.disasm(g.read(a,e-a),a):
        for h in re.findall(r'0x([0-9a-f]{6,8})\b',ins.op_str):
            t=g.at(int(h,16))
            if t and t[0]==int(h,16) and lib(t[2]) in XDK and lib(t[2])!='LIBCMT':
                calls[(lib(t[2]),t[1])].add(o)
cur=None
for (l,n),s in sorted(calls.items()):
    if l!=cur: print("\n==",l); cur=l
    print('  ',n,len(s),sorted(s)[:4])
