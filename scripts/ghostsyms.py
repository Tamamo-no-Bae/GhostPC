# Shared helpers: load Ghost.exe + Ghost.map symbols
import pefile,re,bisect
class Img:
    def __init__(s,exe,mp):
        s.pe=pefile.PE(exe,fast_load=True); s.base=s.pe.OPTIONAL_HEADER.ImageBase
        s.img=s.pe.get_memory_mapped_image()
        s.syms=[]
        for l in open(mp,errors='ignore'):
            m=re.match(r'\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s+(f\s+)?(i\s+)?(\S+)?',l)
            if m and m.group(1)!='0000': s.syms.append((int(m.group(4),16),m.group(3),(m.group(7) or '').strip(), bool(m.group(5))))
        s.syms.sort(); s.addrs=[a for a,*_ in s.syms]
        s.byname={n:a for a,n,*_ in s.syms}
    def at(s,va):
        i=bisect.bisect_right(s.addrs,va)-1
        return s.syms[i] if i>=0 else None
    def funcs(s):
        # (start,end,name,obj) for function symbols
        fl=[x for x in s.syms if x[3]]
        for k,(a,n,o,f) in enumerate(fl):
            nxt=s.addrs[bisect.bisect_right(s.addrs,a)] if bisect.bisect_right(s.addrs,a)<len(s.addrs) else a+16
            yield a,nxt,n,o
    def read(s,va,n): r=va-s.base; return s.img[r:r+n]
