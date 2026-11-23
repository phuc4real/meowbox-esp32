"""Icon (16-256 px) and the Windows version resource for the exe files, from meowbox_agent/__init__.py."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from meowbox_agent import __version__  # noqa: E402
from meowbox_agent.tray import cat_icon  # noqa: E402

out = os.path.join(HERE, "build")
os.makedirs(out, exist_ok=True)
big = cat_icon(256)
big.save(os.path.join(out, "meowbox.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
v = tuple(int(x) for x in __version__.split(".")) + (0,)
open(os.path.join(out, "version_info.txt"), "w", encoding="utf-8").write(f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={v}, prodvers={v}, mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[StringFileInfo([StringTable('040904B0', [
    StringStruct('CompanyName', 'MeowBox'),
    StringStruct('FileDescription', 'MeowBox Agent'),
    StringStruct('FileVersion', '{__version__}'),
    StringStruct('ProductName', 'MeowBox Agent'),
    StringStruct('ProductVersion', '{__version__}'),
    StringStruct('LegalCopyright', 'MIT licence')])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])])
""")
print("icon + version_info for", __version__, "->", out)
print(__version__)
