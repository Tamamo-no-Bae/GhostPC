# nsdthings.py level [filter] -- list placed things (modifier lists: u32 count, count x (u32 id, 32-byte value))
# found by scanning for "basedon" entries (id 0) whose name is a Global.not template
import sys,os,struct,re
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import notdump as N
L=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','game','Levels')
names={nm.lower() for nm,_ in N.idx}
def things(level):
    d=open(os.path.join(L,level+'.nsd'),'rb').read()
    out=[]
    for m in re.finditer(rb'\x00\x00\x00\x00([A-Za-z0-9_]{2,31})\x00', d):
        o=m.start(); nm=m.group(1).decode().lower()
        if nm not in names or o<4: continue
        c=struct.unpack_from('<I',d,o-4)[0]
        if not 1<=c<=64: continue
        mods={}
        for k in range(c):
            p=o+k*36
            if p+36>len(d): break
            mid=struct.unpack_from('<I',d,p)[0]
            nmod,t=N.mods.get(mid,('?%d'%mid,-1)); v=d[p+4:p+36]
            if t==2: val=struct.unpack_from('<f',v)[0]
            elif t==3: val=struct.unpack_from('<i',v)[0]
            elif t==4: val=struct.unpack_from('<3f',v)
            else: val=v.split(b'\0')[0].decode('latin1')
            mods.setdefault(nmod,val)
        out.append((o+4,nm,mods))
    return d,out
if __name__=='__main__':
    d,t=things(sys.argv[1])
    flt=sys.argv[2].lower() if len(sys.argv)>2 else ''
    for o,nm,m in t:
        if flt in nm:
            pos=m.get('position'); print(hex(o), nm, tuple(round(x,1) for x in pos) if isinstance(pos,tuple) else pos,
                  {k:v for k,v in m.items() if k not in ('basedon','position')})
