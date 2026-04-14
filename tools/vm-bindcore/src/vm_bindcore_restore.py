#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vm-bindcore restore service helper
# Restores vCPU pinning configurations after host reboot.

"""
Systemd service helper: restores persisted vCPU pinning configs.
Invoked by vm-bindcore-restore.service after libvirtd is ready.
"""

import subprocess
import sys


def main():
    """Invoke vm-bindcore restore for all saved domains."""
    try:
        result = subprocess.run(
            ["vm-bindcore", "restore"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        return result.returncode
    except FileNotFoundError:
        print("[ERROR] vm-bindcore not found in PATH", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired:
        print("[ERROR] vm-bindcore restore timed out", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
