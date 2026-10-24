#!/usr/bin/env python3
import subprocess
import sys

def main():
    qemu_cmd = sys.argv[1:]
    if not qemu_cmd:
        print("Usage: run_qemu_test.py <qemu_cmd...>")
        sys.exit(1)

    proc = subprocess.run(qemu_cmd)
    # QEMU with isa-debug-exit (val 0x10): exit code = (0x10 << 1) | 1 = 33
    # QEMU with isa-debug-exit (val 0x11): exit code = (0x11 << 1) | 1 = 35
    if proc.returncode == 33:
        # All tests passed!
        sys.exit(0)
    elif proc.returncode == 35:
        print("[run_qemu_test] Kernel self-tests reported failure (exit code 35).")
        sys.exit(1)
    else:
        # Other error or abnormal termination
        sys.exit(proc.returncode)

if __name__ == "__main__":
    main()
