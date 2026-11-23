# PyInstaller spec (docs/firmware_v0.2.0.md, 2.10): one --onedir folder with two executables sharing _internal/:
#   MeowBoxAgent.exe  windowed, tray app
#   meowbox.exe       console, CLI
# Build with build.ps1 (it also makes the icon). onedir, not onefile: starts faster and trips antivirus less.
import os

from PyInstaller.utils.hooks import collect_submodules

HERE = os.path.abspath(SPECPATH)
ROOT = os.path.dirname(HERE)
ICON = os.path.join(HERE, "build", "meowbox.ico")

hidden = (
    collect_submodules("winrt.windows.media.control")
    + collect_submodules("winrt.windows.media.speechsynthesis")
    + collect_submodules("winrt.windows.storage.streams")
    + collect_submodules("winrt.windows.foundation")
    + collect_submodules("zeroconf")
    + ["pystray._win32", "pycaw.pycaw", "comtypes.stream", "PIL.ImageTk"]
)

a = Analysis(
    [os.path.join(HERE, "entry.py")],
    pathex=[ROOT],
    hiddenimports=hidden,
    excludes=["matplotlib", "numpy", "pytest"],
    noarchive=False,
)
pyz = PYZ(a.pure)

agent = EXE(pyz, a.scripts, [], exclude_binaries=True, name="MeowBoxAgent", console=False, icon=ICON,
            version=os.path.join(HERE, "build", "version_info.txt"))
cli = EXE(pyz, a.scripts, [], exclude_binaries=True, name="meowbox", console=True, icon=ICON,
          version=os.path.join(HERE, "build", "version_info.txt"))
COLLECT(agent, cli, a.binaries, a.datas, name="MeowBoxAgent")
