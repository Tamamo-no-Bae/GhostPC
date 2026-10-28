# Find privileged instructions in code that runs unmodified (non-replaced libs)
import sys,os,capstone,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from ghostsyms import Img
g=Img('game/Ghost.exe','game/Ghost.map')
REPL=('d3d8:','dsound:','xacteng:','xapilib:','xnet:','xboxkrnl:')
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
PRIV={'wbinvd','invd','cli','sti','hlt','rdmsr','wrmsr','in','out','insb','insd','outsb','outsd','lidt','lgdt','invlpg','rdpmc','clts','lmsw','ltr','lldt'}
hits=collections.defaultdict(list)
for a,e,n,o in g.funcs():
    if o.startswith(REPL): continue
    for ins in md.disasm(g.read(a,min(e-a,0x40000)),a):
        if ins.mnemonic in PRIV or (ins.mnemonic=='mov' and ('cr' in ins.op_str.split(',')[0] or ', cr' in ins.op_str or 'dr' in ins.op_str.split(',')[0][:3])):
            hits[(ins.mnemonic,ins.bytes.hex())].append('%x %s'%(ins.address,n))
for k,v in hits.items(): print(k,len(v),v[:5])
