#!/usr/bin/env python3
"""Bench access lock — prevents multiple processes from hitting the bench simultaneously.

Usage:
    from bench_lock import acquire_bench, release_bench

    lock = acquire_bench("download_frame")
    if not lock:
        print("Bench is busy", file=sys.stderr)
        sys.exit(1)
    try:
        ... do bench work ...
    finally:
        release_bench(lock)

The lock file lives at ~/workspace/.bench.lock and contains JSON:
    {"pid": 1234, "started": "2026-09-30T02:16:00", "task": "download_frame", "host": "hatch-vm"}

Stale locks (PID no longer alive) are automatically cleared.
"""
import json, os, sys, time, atexit

LOCK_PATH = os.path.expanduser("~/workspace/.bench.lock")

def _pid_alive(pid):
    """True if PID exists and is not a zombie."""
    try:
        os.kill(pid, 0)
    except (OSError, ProcessLookupError):
        return False
    # Check for zombie state via /proc
    try:
        with open(f"/proc/{pid}/stat") as f:
            state = f.read().split()[2]
        if state == "Z":
            return False  # zombie — treat as dead
    except:
        pass
    return True

def check_bench():
    """Returns (busy: bool, info: dict|None)."""
    if not os.path.exists(LOCK_PATH):
        return False, None
    try:
        with open(LOCK_PATH) as f:
            info = json.load(f)
    except:
        return False, None
    pid = info.get("pid")
    if pid and _pid_alive(pid):
        return True, info
    # Stale lock — clear it
    try:
        os.remove(LOCK_PATH)
    except:
        pass
    return False, None

def acquire_bench(task="unknown"):
    """Try to acquire the bench lock. Returns lock info dict on success, None if busy."""
    busy, info = check_bench()
    if busy:
        return None
    lock_info = {
        "pid": os.getpid(),
        "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "task": task,
        "host": os.uname().nodename,
    }
    # Atomic write via temp + rename
    tmp = LOCK_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(lock_info, f)
    os.rename(tmp, LOCK_PATH)
    # Auto-release on exit
    atexit.register(lambda: release_bench(lock_info))
    return lock_info

def release_bench(lock_info=None):
    """Release the bench lock (only if we hold it)."""
    if not os.path.exists(LOCK_PATH):
        return
    try:
        with open(LOCK_PATH) as f:
            current = json.load(f)
    except:
        return
    # Only remove if it's our lock
    if lock_info and current.get("pid") != lock_info.get("pid"):
        return
    if not lock_info and current.get("pid") != os.getpid():
        return
    try:
        os.remove(LOCK_PATH)
    except:
        pass

if __name__ == "__main__":
    # CLI: check status, or --clear to force-release
    if len(sys.argv) > 1 and sys.argv[1] in ("--clear", "--force", "clear"):
        if os.path.exists(LOCK_PATH):
            try:
                with open(LOCK_PATH) as f:
                    info = json.load(f)
                print(f"Force-clearing lock: pid={info.get('pid')} task={info.get('task')}")
            except:
                print("Force-clearing lock (unreadable)")
            os.remove(LOCK_PATH)
            print("Lock cleared")
        else:
            print("No lock to clear")
        sys.exit(0)
    busy, info = check_bench()
    if busy:
        print(f"BENCH BUSY: pid={info['pid']} task={info['task']} since={info['started']}")
        sys.exit(1)
    else:
        print("Bench is free")
        sys.exit(0)
