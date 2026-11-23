"""Compatibility entry point: `python agent.py <command>` keeps working (install_autostart.ps1, old notes).
The code lives in the meowbox_agent package; see meowbox_agent/__init__.py."""
from meowbox_agent.cli import main

if __name__ == "__main__":
    main()
