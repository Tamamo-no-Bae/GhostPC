# notdump.py [name...] -- list/decode templates in Misc/Templates/Global.not
# index: u32 'NOT\x10', u32 1, u32 n, n x (name[32], u32 offset); record: u32 count, count x (u32 modifier id, 32-byte value)
# modifier names/types: re/modifiers.txt (dumped from gameTemplate_aModifiers; arg 0 string, 2 float, 3 int, 4 vector)
import struct,sys,os
H=os.path.dirname(os.path.abspath(__file__))
D=os.path.join(os.environ.get('GHOST_GAME_DIR') or os.path.join(H,'..','game'),'Misc','Templates','Global.not')
d=open(D,'rb').read()
mods={}
MODS=os.path.join(H,'modifiers.txt') if os.path.exists(os.path.join(H,'modifiers.txt')) else os.path.join(H,'..','re','modifiers.txt')
for l in open(MODS):
    s=l.split(); mods[int(s[0])]=(' '.join(s[2:-2]),int(s[-1][3:]))
n=struct.unpack_from('<I',d,8)[0]
idx=[(d[12+i*36:12+i*36+32].split(b'\0')[0].decode('latin1'), struct.unpack_from('<I',d,12+i*36+32)[0]) for i in range(n)]
def props(off):
    c=struct.unpack_from('<I',d,off)[0]; out=[]
    for k in range(c):
        p=off+4+k*36; mid=struct.unpack_from('<I',d,p)[0]; v=d[p+4:p+36]
        nm,t=mods.get(mid,('?%d'%mid,-1))
        if t==2: val='%g'%struct.unpack_from('<f',v)[0]
        elif t==3: val=str(struct.unpack_from('<i',v)[0])
        elif t==4: val='(%g %g %g)'%struct.unpack_from('<3f',v)
        else: val=v.split(b'\0')[0].decode('latin1')
        out.append((nm,val))
    return out
def find(q): return next((off for nm,off in idx if nm.lower()==q.lower()),None)
if __name__=='__main__':
    if len(sys.argv)==1:
        for nm,off in idx: print(nm)
    for q in sys.argv[1:]:
        off=find(q); print('==',q)
        for nm,val in props(off): print('   %-22s %s'%(nm,val))
