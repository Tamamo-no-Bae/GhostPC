# Find D3D__RenderState / D3D__TextureState indices written by each SetRenderState_*/SetTextureState_* function
import sys,os,re,capstone
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
g=Img('game/Ghost.exe','game/Ghost.map')
rs=g.byname['_D3D__RenderState']; ts=g.byname['_D3D__TextureState']; df=g.byname['_D3D__DirtyFlags']
print('RenderState %x TextureState %x DirtyFlags %x'%(rs,ts,df))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
for a,e,n,o in g.funcs():
    if not o.startswith('d3d8:'): continue
    if not re.search(r'SetRenderState|SetTextureState|SetPixelShader|SetVertexShader@|SetTexture@|SetTransform|SetScissors|SetViewport',n): continue
    hits=[]
    for ins in md.disasm(g.read(a,min(e-a,0x400)),a):
        for h in re.findall(r'0x([0-9a-f]{6,8})',ins.op_str):
            v=int(h,16)
            if rs<=v<rs+0x400: hits.append('RS[%d]%s'%((v-rs)//4,'w' if ins.op_str.startswith('dword ptr [0x'+h) else ''))
            elif ts<=v<ts+0x300: hits.append('TS[%d]'%((v-ts)//4))
            elif v==df: hits.append('DIRTY')
    print(n,' '.join(dict.fromkeys(hits)))
