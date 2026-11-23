"""Find l32r instructions (IROM) that load from the given literal-pool addresses.
Usage: python find_l32r.py 42003ef4 42003f58 ..."""
import sys,os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from fwimage import *
la,ln,fo=[s for s in segs if s[0]==0x42000020][0]
blob=d[fo:fo+ln]
targets={int(x,16) for x in sys.argv[1:]}
for i in range(0,min(ln,0x40000+0x4000)):
    if blob[i]&0xf!=1: continue
    pc=la+i; imm=(blob[i+1]|blob[i+2]<<8)-0x10000
    t=((pc+3)&~3)+(imm<<2)
    if t in targets: print(hex(t),'<- l32r at',hex(pc))
