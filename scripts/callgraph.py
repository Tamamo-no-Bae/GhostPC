# Build call/reference graph of Ghost.exe functions; cache as pickle
import sys,os,capstone,re,pickle,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
D=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','game')
CACHE=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','re','callgraph.pkl')
def load():
    if os.path.exists(CACHE): return pickle.load(open(CACHE,'rb'))
    g=Img(os.path.join(D,'Ghost.exe'),os.path.join(D,'Ghost.map'))
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
    funcs={}; refs=collections.defaultdict(set)
    symname={a:n for a,n,*_ in g.syms}; obj={n:o for a,n,o,f in g.syms}
    for a,e,n,o in g.funcs():
        funcs[n]=(a,e,o)
        if e-a>0x40000: e=a+0x40000
        for ins in md.disasm(g.read(a,e-a),a):
            for h in re.findall(r'0x([0-9a-f]{5,8})\b',ins.op_str):
                v=int(h,16)
                if v in symname and symname[v]!=n: refs[n].add(symname[v])
    r=(funcs,dict(refs),obj)
    pickle.dump(r,open(CACHE,'wb')); return r
if __name__=='__main__':
    funcs,refs,obj=load(); print(len(funcs),'funcs',sum(len(v) for v in refs.values()),'edges')
