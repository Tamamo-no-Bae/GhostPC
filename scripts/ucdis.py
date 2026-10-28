# Disassemble NV2A vertex microcode from a GhostPC vs dump (test/vsNNN.hlsl) into vs-like text
import sys,re
t=open(sys.argv[1]).read()
uc=[int(x,16) for w in re.findall(r'^// ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})$',t,re.M) for x in w]
def F(w,word,lo,bits): return (w[word]>>lo)&((1<<bits)-1)
MAC=['nop','mov','mul','add','mad','dp3','dph','dp4','dst','min','max','slt','sge','arl']
ILU=['nop','mov','rcp','rcc','rsq','exp','log','lit']
OUT={0:'oPos',3:'oD0',4:'oD1',5:'oFog',6:'oPts',7:'oB0',8:'oB1',9:'oT0',10:'oT1',11:'oT2',12:'oT3'}
def mask(m): return ''.join(c for i,c in enumerate('xyzw') if m&(8>>i))
for k in range(len(uc)//4):
    w=uc[k*4:k*4+4]
    a0x=F(w,3,1,1)
    def src(which):
        if which==0: neg,sw,r,mux=F(w,1,8,1),[F(w,1,6,2),F(w,1,4,2),F(w,1,2,2),F(w,1,0,2)],F(w,2,28,4),F(w,2,26,2)
        elif which==1: neg,sw,r,mux=F(w,2,25,1),[F(w,2,23,2),F(w,2,21,2),F(w,2,19,2),F(w,2,17,2)],F(w,2,13,4),F(w,2,11,2)
        else: neg,sw,r,mux=F(w,2,10,1),[F(w,2,8,2),F(w,2,6,2),F(w,2,4,2),F(w,2,2,2)],(F(w,2,0,2)<<2)|F(w,3,30,2),F(w,3,28,2)
        b={1:'r%d'%r,2:'v%d'%F(w,1,9,4),3:('c[a0+%d]' if a0x else 'c%d')%(F(w,1,13,8)-96)}.get(mux,'?')
        s=''.join('xyzw'[x] for x in sw)
        return ('-' if neg else '')+b+('' if s=='xyzw' else '.'+s)
    mac,ilu=F(w,1,21,4),F(w,1,25,3)
    mm,outR,im=F(w,3,24,4),F(w,3,20,4),F(w,3,16,4)
    om,orb,oa,omux=F(w,3,12,4),F(w,3,11,1),F(w,3,3,8),F(w,3,2,1)
    parts=[]
    if mac:
        dst=[]
        if mm: dst.append('r%d.%s'%(outR,mask(mm)))
        if om and not omux: dst.append('%s.%s'%(OUT.get(oa,'c%d'%oa) if orb else 'c%d'%(oa-96),mask(om)))
        args={1:[0],2:[0,1],3:[0,2],4:[0,1,2],5:[0,1],6:[0,1],7:[0,1],8:[0,1],9:[0,1],10:[0,1],11:[0,1],12:[0,1],13:[0]}[mac]
        parts.append('%s %s, %s'%(MAC[mac],'/'.join(dst) or 'a0' if mac==13 else '/'.join(dst),', '.join(src(a) for a in args)))
    if ilu:
        dst=[]
        if im: dst.append('r%d.%s'%(1 if mac else outR,mask(im)))
        if om and omux: dst.append('%s.%s'%(OUT.get(oa,'c%d'%oa) if orb else 'c%d'%(oa-96),mask(om)))
        parts.append('%s %s, %s'%(ILU[ilu],'/'.join(dst),src(2)))
    print('%3d  %s'%(k,'  +  '.join(parts)))
    if F(w,3,0,1): break
