# Compare NV2A microcode (my decoding rules) vs the PC vs_1_1 version of the same shader on captured state.
import struct,sys,re,math,os,subprocess
import numpy as np
S=open('test/vsstate.bin','rb').read()
C=np.array(struct.unpack_from('<768f',S,0),dtype=np.float64).reshape(192,4)
regoff=struct.unpack_from('<16i',S,3072); stride=struct.unpack_from('<I',S,3136)[0]
V=[]
for vi in range(4):
    base=3140+vi*stride
    V.append({r:np.array(struct.unpack_from('<4f',S,base+o)) for r,o in enumerate(regoff) if o>=0})
SW='xyzw'
def F(w,word,lo,bits): return (w[word]>>lo)&((1<<bits)-1)
def run_ucode(uc,v):
    R=[np.zeros(4) for _ in range(16)]; O={}; a0=0
    for k in range(len(uc)//4):
        w=uc[k*4:k*4+4]
        def src(which):
            if which==0: neg,sw,r,mux=F(w,1,8,1),[F(w,1,6,2),F(w,1,4,2),F(w,1,2,2),F(w,1,0,2)],F(w,2,28,4),F(w,2,26,2)
            elif which==1: neg,sw,r,mux=F(w,2,25,1),[F(w,2,23,2),F(w,2,21,2),F(w,2,19,2),F(w,2,17,2)],F(w,2,13,4),F(w,2,11,2)
            else: neg,sw,r,mux=F(w,2,10,1),[F(w,2,8,2),F(w,2,6,2),F(w,2,4,2),F(w,2,2,2)],(F(w,2,0,2)<<2)|F(w,3,30,2),F(w,3,28,2)
            if mux==1: x=R[r]
            elif mux==2: x=v.get(F(w,1,9,4),np.array([0,0,0,1.]))
            elif mux==3: x=C[(F(w,1,13,8)+(a0 if F(w,3,1,1) else 0))%192]
            else: x=np.zeros(4)
            x=x[sw]; return -x if neg else x
        mac,ilu=F(w,1,21,4),F(w,1,25,3)
        A,B,Cc=src(0),src(1),src(2)
        m=l=None
        if mac==1 or mac==13: m=A
        elif mac==2: m=A*B
        elif mac==3: m=A+Cc
        elif mac==4: m=A*B+Cc
        elif mac==5: m=np.full(4,A[:3]@B[:3])
        elif mac==6: m=np.full(4,A[:3]@B[:3]+B[3])
        elif mac==7: m=np.full(4,A@B)
        elif mac==8: m=np.array([1,A[1]*B[1],A[2],B[3]])
        elif mac==9: m=np.minimum(A,B)
        elif mac==10: m=np.maximum(A,B)
        elif mac==11: m=(A<B)*1.
        elif mac==12: m=(A>=B)*1.
        x=Cc[0]
        if ilu==1: l=Cc
        elif ilu in(2,3): l=np.full(4,1/x if x else math.inf)
        elif ilu==4: l=np.full(4,1/math.sqrt(abs(x)) if x else math.inf)
        elif ilu==5: f=math.floor(x); l=np.array([2**f,x-f,2**x,1])
        elif ilu==6: a=abs(x); e=math.floor(math.log2(a)) if a else -1e30; l=np.array([e,a/2**e if a else 1,math.log2(a) if a else -1e30,1])
        elif ilu==7:
            l=np.array([1,0,0,1.]);
            if Cc[0]>0: l[1]=Cc[0]; l[2]=Cc[1]**Cc[3] if Cc[1]>0 else 0
        mm,outR,im=F(w,3,24,4),F(w,3,20,4),F(w,3,16,4)
        om,orb,oa,omux=F(w,3,12,4),F(w,3,11,1),F(w,3,3,8),F(w,3,2,1)
        def wr(dst,val,mask):
            for i in range(4):
                if mask&(8>>i): dst[i]=val[i]
        if mac==13: a0=int(math.floor(m[0]+1e-4))
        elif mac and mm: wr(R[outR],m,mm)
        if ilu and im: wr(R[1 if mac else outR],l,im)
        if om and orb:
            val=l if omux else m
            o=O.setdefault(oa,np.array([0,0,0,1.]))
            wr(o,val,om)
            if oa==0: wr(R[12],val,om)
        if F(w,3,0,1): break
    return O,R
def run_pc(lines,v):
    R=[np.zeros(4) for _ in range(12)]; O={}; a0=0
    def rd(t):
        neg=t.startswith('-'); t=t.lstrip('-')
        m=re.match(r'([rvc]|oPos|oD|oT|oFog)(\d*)(?:\[a0\.x\])?(?:\.([xyzw]+))?$',t)
        k,i,sw=m.group(1),int(m.group(2) or 0),m.group(3) or 'xyzw'
        sw=(sw+sw[-1]*4)[:4]
        if k=='r': x=R[i]
        elif k=='v': x=v.get(i,np.array([0,0,0,1.]))
        elif k=='c': x=C[i+96+(a0 if '[a0' in t else 0)]
        x=np.array([x[SW.index(c)] for c in sw]); return -x if neg else x
    for ln in lines:
        p=ln.split(None,1)
        if len(p)<2 or p[0].startswith('vs_') or p[0].startswith('//'): continue
        op=p[0]; args=[a.strip() for a in p[1].split(',')]
        d=args[0]; s=[rd(a) for a in args[1:]]
        if op=='mov': val=s[0]
        elif op=='mul': val=s[0]*s[1]
        elif op=='add': val=s[0]+s[1]
        elif op=='mad': val=s[0]*s[1]+s[2]
        elif op=='dp3': val=np.full(4,s[0][:3]@s[1][:3])
        elif op=='dp4': val=np.full(4,s[0]@s[1])
        elif op=='rsq': val=np.full(4,1/math.sqrt(abs(s[0][3 if '.w' in args[1] else 0])) if True else 0)
        elif op=='rcp': x=s[0][0]; val=np.full(4,1/x if x else math.inf)
        elif op=='max': val=np.maximum(s[0],s[1])
        elif op=='min': val=np.minimum(s[0],s[1])
        elif op=='dst': val=np.array([1,s[0][1]*s[1][1],s[0][2],s[1][3]])
        else: print('?',op); continue
        m=re.match(r'(r|oPos|oD|oT|oFog|a)(\d*)(?:\.([xyzw]+))?',d)
        k,i,mask=m.group(1),int(m.group(2) or 0),m.group(3) or 'xyzw'
        if k=='a': a0=int(math.floor(val[0]+1e-4)); continue
        dst=R[i] if k=='r' else O.setdefault(k+str(i),np.array([0,0,0,1.]))
        for c in mask: dst[SW.index(c)]=val[SW.index(c)]
    return O
uc=None
import glob
for f in glob.glob('test/vs*.hlsl'):
    t=open(f).read()
    if 'handle' in t and ' %s instructions'%sys.argv[1] in t:
        uc=[int(x,16) for w in re.findall(r'^// ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})$',t,re.M) for x in w]
dis=subprocess.run([sys.executable,'scripts/vsdis.py','game/VertexShaders/'+sys.argv[2]],capture_output=True,text=True).stdout.splitlines()
for vi in range(4):
    O,R=run_ucode(uc,V[vi]); P=run_pc(dis,V[vi])
    print('vertex',vi,'in',{k:np.round(x,3).tolist() for k,x in V[vi].items()})
    print('  ucode oD0',np.round(O.get(3),3),' r12',np.round(R[12],2))
    print('  pc    oD0',np.round(P.get('oD0'),3),' oPos',np.round(P.get('oPos0'),2))
