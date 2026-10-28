# Disassemble a PC D3D8 vs.1.1 token shader (.xvu in VertexShaders\) with d3dcompiler_47
import ctypes,sys
d=open(sys.argv[1],'rb').read()
dc=ctypes.WinDLL('d3dcompiler_47.dll')
blob=ctypes.c_void_p()
hr=dc.D3DDisassemble(d,len(d),0,None,ctypes.byref(blob))
if hr: print('hr %x'%(hr&0xffffffff)); sys.exit()
vt=ctypes.cast(blob,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
GetPtr=ctypes.WINFUNCTYPE(ctypes.c_void_p,ctypes.c_void_p)(vt[3]); GetSize=ctypes.WINFUNCTYPE(ctypes.c_size_t,ctypes.c_void_p)(vt[4])
print(ctypes.string_at(GetPtr(blob),GetSize(blob)).decode(errors='replace'))
