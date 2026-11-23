"""Helpers to load an ESP32-S3 app image (segments -> memory map), read memory, find strings/words. Used by the other tools.
Set FW_APP to the app image path (default ../backup/ota0_app.bin)."""
import struct,re,capstone
import os
APP=os.environ.get('FW_APP', os.path.join(os.path.dirname(__file__), '..', 'backup', 'ota0_app.bin'))
d=open(APP,'rb').read()
segs=[]
n=d[1]; off=24
for i in range(n):
    la,ln=struct.unpack('<II',d[off:off+8]); segs.append((la,ln,off+8)); off+=8+ln
def rd(addr,n):
    for la,ln,fo in segs:
        if la<=addr<la+ln: return d[fo+addr-la:fo+addr-la+n]
    return None
def u32(a): b=rd(a,4); return struct.unpack('<I',b)[0] if b else None
def cstr(a):
    b=rd(a,300); return b.split(b'\0')[0].decode(errors='replace') if b else None
def find_str(s):
    out=[]
    for la,ln,fo in segs:
        blob=d[fo:fo+ln]
        for m in re.finditer(re.escape(s.encode()),blob): out.append(la+m.start())
    return out
def find_word(v):
    out=[]; pat=struct.pack('<I',v)
    for la,ln,fo in segs:
        blob=d[fo:fo+ln]; i=blob.find(pat)
        while i!=-1:
            if i%4==0: out.append(la+i)
            i=blob.find(pat,i+1)
    return out
CODE=[s for s in segs if s[0]>=0x40000000 and s[0]<0x50000000]
md=capstone.Cs(capstone.CS_ARCH_XTENSA,capstone.CS_MODE_XTENSA_ESP32S3 if hasattr(capstone,'CS_MODE_XTENSA_ESP32S3') else 0)
md.skipdata=False
def dis(addr,n):
    b=rd(addr,n); return list(md.disasm(b,addr)) if b else []
def l32r_target(ins):
    # l32r target = ((pc+3)&~3) + (imm16 sign-extended<<2), imm16 negative
    b=ins.bytes
    if len(b)==3 and (b[0]&0xf)==1:
        imm=b[1]|(b[2]<<8); imm=imm-0x10000
        return ((ins.address+3)&~3)+(imm<<2)
    return None
