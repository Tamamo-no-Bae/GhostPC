# Execute a generated vertex HLSL (the simple subset GhostPC emits) in Python on captured state
import struct,sys,re,math,glob
import numpy as np
exec(open('scripts/vscheck.py').read().split('SW=')[0])   # loads C, V
class Vec:
    def __init__(s,a): object.__setattr__(s,'a',np.array(a,dtype=np.float64).reshape(-1))
    def _i(n): return ['xyzw'.index(c) for c in n]
    def __getattr__(s,n):
        if n and all(c in 'xyzw' for c in n):
            a=s.a if len(s.a)==4 else np.repeat(s.a,4)
            return Vec(a[Vec._i(n)])
        raise AttributeError(n)
    def __setattr__(s,n,v):
        idx=Vec._i(n); va=v.a if isinstance(v,Vec) else np.full(len(idx),v)
        for k,i in enumerate(idx): s.a[i]=va[k if len(va)>1 else 0]
    def __add__(s,o): return Vec(s.a+(o.a if isinstance(o,Vec) else o))
    __radd__=__add__
    def __sub__(s,o): return Vec(s.a-(o.a if isinstance(o,Vec) else o))
    def __rsub__(s,o): return Vec((o.a if isinstance(o,Vec) else o)-s.a)
    def __mul__(s,o): return Vec(s.a*(o.a if isinstance(o,Vec) else o))
    __rmul__=__mul__
    def __neg__(s): return Vec(-s.a)
    def __lt__(s,o): return Vec((s.a<o.a)*1.)
    def __ge__(s,o): return Vec((s.a>=o.a)*1.)
def float4(*xs):
    if len(xs)==1: x=xs[0]; return Vec(x.a if isinstance(x,Vec) and len(x.a)==4 else np.full(4,x.a[0] if isinstance(x,Vec) else x))
    return Vec([x.a[0] if isinstance(x,Vec) else x for x in xs])
def dot(a,b): d=float(a.a@b.a); return Vec([d]*4)
def mn(a,b): return Vec(np.minimum(a.a,b.a))
def mx(a,b): return Vec(np.maximum(a.a,b.a))
def rcp_(x): x=x.a[0]; return Vec([1/x if x else math.inf]*4)
rcc_=rcp_
def rsq_(x): x=x.a[0]; return Vec([1/math.sqrt(abs(x)) if x else math.inf]*4)
def floor(x): return math.floor(x.a[0])
class Inp:
    def __init__(s,v): s.v=v
    def __getattr__(s,n): return Vec(s.v.get(int(n[1:]),np.array([0,0,0,1.])).copy())
src=open(sys.argv[1]).read()
body=src[src.index('VOUT main'):]
body=body[body.index('\n')+1:body.rindex('return o;')]
code=[]
for ln in body.splitlines():
    ln=ln.strip().rstrip(';')
    if not ln or ln.startswith('VOUT') or ln.startswith('o.') or ln.startswith('float4 p'): continue
    ln=re.sub(r'^float4 ','',ln); ln=re.sub(r'^int ','',ln)
    ln=ln.replace('(int)floor','floor').replace('min(','mn(').replace('max(','mx(')
    ln=re.sub(r'c\[([^\]]+)\]',r'Vec(C[(\1)%192])',ln)
    ln='\n'.join(re.split(r', (?=\w+ = )',ln))   # "a = 0, b = 0" decls
    code.append(ln)
code='\n'.join(code)
code=code.replace('= 0\n','= Vec([0,0,0,0])\n')
code=re.sub(r'= 0$','= Vec([0,0,0,0])',code,flags=re.M)
code=re.sub(r'^(r\d+|m|l|oPos|oD0|oD1|oFog|oPts|oB0|oB1|oT\d) = (?!Vec)',r'\1 = ',code,flags=re.M)
code=code.replace('a0 = Vec([0,0,0,0])','a0 = 0')
code=re.sub(r'r12\.(\w+) = oPos\.(\w+) = (.+)',r'r12.\1 = \3\noPos.\2 = \3',code)
for vi in range(4):
    env={'i':Inp(V[vi]),'Vec':Vec,'C':C,'float4':float4,'dot':dot,'mn':mn,'mx':mx,'rcp_':rcp_,'rcc_':rcc_,'rsq_':rsq_,'floor':floor}
    try: exec(code,env)
    except Exception as e: print("error",e); print(code[:600]); break
    print('vertex',vi,'oD0',np.round(env['oD0'].a,3),'r12',np.round(env['r12'].a,1))
