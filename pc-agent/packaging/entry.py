"""Entry point for both executables built by meowbox.spec.

MeowBoxAgent.exe (windowed): no arguments -> tray app; arguments -> CLI (output goes to the log).
meowbox.exe (console): the CLI (`meowbox notify "Build xong"`, `meowbox say ...`, `meowbox discover`).
"""
import sys

from meowbox_agent.cli import main

if __name__ == "__main__":
    if len(sys.argv) == 1 and sys.stdout is None:  # windowed exe started from the Start menu / at logon
        sys.argv.append("tray")
    main()
