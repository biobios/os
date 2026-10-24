#!/usr/bin/env python3
import os
import subprocess
import sys

DEFAULT_TIMEOUT_SEC = 45

def main():
    args = sys.argv[1:]
    if not args:
        print("Usage: run_qemu_test.py [--timeout <sec>] <qemu_cmd...>")
        sys.exit(1)

    timeout = float(os.environ.get("QEMU_TEST_TIMEOUT", DEFAULT_TIMEOUT_SEC))

    if len(args) >= 2 and args[0] == "--timeout":
        timeout = float(args[1])
        qemu_cmd = args[2:]
    else:
        qemu_cmd = args

    if not qemu_cmd:
        print("Error: No QEMU command specified.")
        sys.exit(1)

    try:
        proc = subprocess.run(qemu_cmd, timeout=timeout)
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
            print(f"[run_qemu_test] QEMU terminated with unexpected return code: {proc.returncode}")
            sys.exit(proc.returncode)

    except subprocess.TimeoutExpired as exc:
        print(f"\n[run_qemu_test] ERROR: Test timed out after {timeout:.1f} seconds! Terminating QEMU...")
        sys.exit(124)
    except KeyboardInterrupt:
        print("\n[run_qemu_test] Interrupted by user.")
        sys.exit(130)

if __name__ == "__main__":
    main()
