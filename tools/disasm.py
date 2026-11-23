"""Xtensa (ESP32-S3) disassembler with l32r literal + string annotation. Needs capstone>=6 (pip install --pre capstone==6.0.0a11).
Usage: python disasm.py <start_hex> <end_hex> > out.txt"""
import sys,os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from fwimage import *
sys.stdout.reconfigure(encoding='utf-8')
def ilen(b0):
    op0=b0&0xf
    return 2 if 8<=op0<=0xd else (4 if op0==0xe or op0==0xf else 3)
a=int(sys.argv[1],16); end=int(sys.argv[2],16)
while a<end:
    got=False
    for ins in md.disasm(rd(a,end-a),a):
        got=True; a=ins.address+ins.size
        note=''
        t=l32r_target(ins) if ins.mnemonic=='l32r' else None
        if t is not None:
            v=u32(t) or 0; s=cstr(v) if 0x3c000000<=v<0x3e000000 else None
            note=f'   ; =0x{v:08x}'+(f' "{s[:70]}"' if s and len(s)>2 and s.isprintable() else '')
        print(f'{ins.address:08x}: {ins.mnemonic:8} {ins.op_str}{note}')
    if a>=end: break
    n=ilen(rd(a,1)[0]); print(f'{a:08x}: ??       {rd(a,n).hex()}'); a+=n
