"""Find strings in the app image and the IROM literal-pool slots that point at them.
Usage: python find_string_refs.py "OSTB_XIAOZHI_3ST" "Initialize"   (FW_APP env var = app image, default ../backup/ota0_app.bin)
Then feed the literal addresses to find_l32r.py to locate the code that uses them."""
import sys,os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from fwimage import *
sys.stdout.reconfigure(encoding='utf-8')
def start(a):
    while rd(a-1,1)!=b'\0': a-=1
    return a
seen=set()
for needle in sys.argv[1:]:
    for a0 in find_str(needle):
        a=start(a0)
        if a in seen: continue
        seen.add(a); lits=find_word(a)
        if lits: print(hex(a),repr(cstr(a))[:100],[hex(x) for x in lits])
