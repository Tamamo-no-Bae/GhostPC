import sys,capstone,collections,re
import os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
g=Img(sys.argv[1],sys.argv[2])
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
c=collections.Counter(); objs=collections.defaultdict(set)
mm=collections.Counter(); mmobj=collections.defaultdict(set)
for a,e,n,o in g.funcs():
    if e-a>0x20000: e=a+0x20000
    for ins in md.disasm(g.read(a,e-a),a):
        s=ins.op_str
        if 'fs:' in s:
            k=re.search(r'fs:\[([^\]]+)\]',s).group(1); c[k]+=1; objs[k].add(o)
        for h in re.findall(r'0x(f[cdef][0-9a-f]{6})\b',s):
            mm[h[:3]]+=1; mmobj[h[:3]].add(o)
print("fs offsets:")
for k,v in c.most_common(): print(' ',k,v,sorted(objs[k])[:15])
print("MMIO-ish constants (0xFC..-0xFF..):")
for k,v in mm.most_common(): print(' ',k,v,sorted(mmobj[k])[:20])
