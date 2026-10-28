# dis.py <name-or-hexaddr> [maxbytes]  -- disassemble a Ghost.exe function with map symbols
import sys,os,capstone,re
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
D=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','game')
g=Img(os.path.join(D,'Ghost.exe'),os.path.join(D,'Ghost.map'))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def sym(v):
    t=g.at(v)
    if not t or v-t[0]>0x4000: return None
    return t[1]+('+%x'%(v-t[0]) if v!=t[0] else '')
for q in sys.argv[1:2]:
    a=int(q,16) if re.fullmatch(r'(0x)?[0-9a-fA-F]{6,8}',q) else (g.byname.get(q) or next(a for a,n,*_ in g.syms if q in n))
    t=g.at(a); print('==',t[1],t[2],hex(a))
    i=g.addrs.index(t[0]) if t[0] in g.addrs else 0
    end=g.syms[i+1][0] if a==t[0] and i+1<len(g.syms) else a+int(sys.argv[2] if len(sys.argv)>2 else '200',16)
    if len(sys.argv)>2: end=a+int(sys.argv[2],16)
    for ins in md.disasm(g.read(a,end-a),a):
        c=''
        for h in re.findall(r'0x([0-9a-f]{5,8})\b',ins.op_str):
            s=sym(int(h,16))
            if s: c+=' ; '+s
        print('%08x  %-6s %s%s'%(ins.address,ins.mnemonic,ins.op_str,c))
