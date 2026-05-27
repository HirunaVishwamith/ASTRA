"""
Convenience launcher for the ASTRA GUI.

Use:
  python run.py

This ensures the project root is on sys.path so local modules (orbit/network/routing)
import correctly even if your shell isn't already in the folder.
"""

from __future__ import annotations

import os
import sys


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)

    # Import after sys.path fix
    import main as app_main  # noqa: F401

    # main.py already starts the Qt event loop under __main__.
    # So for run.py we explicitly execute the same entry behavior.
    # If you later refactor main.py to expose an app factory, update this accordingly.
    if hasattr(app_main, "__name__"):
        # Re-run the script entry by executing its __main__ block equivalent:
        # easiest is to call it as a module.
        import runpy

        runpy.run_module("main", run_name="__main__")


if __name__ == "__main__":
    main()

