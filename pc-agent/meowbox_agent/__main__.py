"""python -m meowbox_agent [command]  (no command and a windowed exe -> tray)."""
import sys

from .cli import main

if __name__ == "__main__":
    if len(sys.argv) == 1 and sys.stdout is None:  # pythonw / windowed exe with no arguments: tray
        sys.argv.append("tray")
    main()
