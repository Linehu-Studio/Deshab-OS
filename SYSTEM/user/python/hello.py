"""Minimal CPython readiness check for the Deshab Linux guest."""

from __future__ import annotations

import platform
import sys


def main() -> None:
    version = " ".join(sys.version.split())
    print("DESHAB_PYTHON_READY")
    print(f"version={version}")
    print(f"platform={platform.platform()}")


if __name__ == "__main__":
    main()
