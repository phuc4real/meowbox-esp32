"""Dump NVS (0x9000, 16KB) key/values from a full flash dump. Passwords are masked.
Usage: python dump_nvs.py ../backup/speaker_full_16MB.bin"""
import sys,struct
d=open(sys.argv[1],'rb').read()[0x9000:0xD000]
T={0x01:'u8',0x11:'i8',0x02:'u16',0x12:'i16',0x04:'u32',0x14:'i32',0x08:'u64',0x18:'i64',0x21:'str',0x42:'blob',0x48:'blobidx'}
ns={0:''}
ents=[]
for p in range(0,len(d),4096):
    pg=d[p:p+4096]
    if pg[:4]==b'\xff'*4: continue
    bm=pg[32:64]
    i=0
    while i<126:
        st=(bm[i//4]>>((i%4)*2))&3
        e=pg[64+i*32:64+i*32+32]
        if st!=2: i+=1; continue
        nsi,typ,span=e[0],e[1],e[2]
        key=e[8:24].split(b'\0')[0].decode(errors='replace')
        if nsi==0 and typ==1: ns[e[24]]=key; i+=max(span,1); continue
        val=None
        if typ in(0x01,0x11,0x02,0x12,0x04,0x14,0x08,0x18):
            sz={1:1,2:2,4:4,8:8}[typ&0xf]; fmt={(1,0):'B',(1,1):'b',(2,0):'H',(2,1):'h',(4,0):'I',(4,1):'i',(8,0):'Q',(8,1):'q'}[(sz,typ>>4)]
            val=struct.unpack('<'+fmt,e[24:24+sz])[0]
        elif typ in(0x21,0x42):
            ln=struct.unpack('<H',e[24:26])[0]
            raw=pg[64+(i+1)*32:64+(i+1)*32+ln]
            val=raw.rstrip(b'\0').decode(errors='replace') if typ==0x21 else raw[:48].hex()+('..' if ln>48 else '')
        elif typ==0x48: val='(blob index)'
        ents.append((nsi,key,T.get(typ,hex(typ)),val)); i+=max(span,1)
for nsi,k,t,v in ents:
    if k.lower() in('password','pass','psk') or 'pass' in k.lower(): v='***'
    print(f"[{ns.get(nsi,nsi)}] {k} ({t}) = {v}")
