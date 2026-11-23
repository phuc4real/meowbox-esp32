"""Print the partition table (at 0x8000) of a full flash dump and the app descriptor of each app partition.
Usage: python parse_partitions.py ../backup/speaker_full_16MB.bin"""
import sys,struct
d=open(sys.argv[1],'rb').read()
pt=d[0x8000:0x9000]
T={0:'app',1:'data'}
for i in range(0,0xC00,32):
    e=pt[i:i+32]
    if e[:2]!=b'\xaa\x50': break
    t,st,off,sz=e[2],e[3],*struct.unpack('<II',e[4:12])
    name=e[12:28].split(b'\0')[0].decode()
    print(f"{name:12} type={T.get(t,t)} sub=0x{st:02x} off=0x{off:06x} size=0x{sz:06x} ({sz//1024}KB)")
    if t==0:
        h=d[off:off+0x200]
        # esp_app_desc at offset 0x20 after image header+first segment header (24+8)
        a=d[off+0x20:off+0x20+256]
        if struct.unpack('<I',a[:4])[0]==0xABCD5432:
            ver=a[16:48].split(b'\0')[0]; proj=a[48:80].split(b'\0')[0]
            tm=a[80:96].split(b'\0')[0]; dt=a[96:112].split(b'\0')[0]; idf=a[112:144].split(b'\0')[0]
            print(f"   -> project={proj.decode()} version={ver.decode()} idf={idf.decode()} built={dt.decode()} {tm.decode()}")
        else: print("   -> (empty / no app desc)", h[:4].hex())
