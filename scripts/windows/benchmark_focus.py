"""Bounded foreground acquisition for a game launched by a benchmark harness."""

import ctypes
from ctypes import wintypes
import time


def foreground_pid():
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    pid = wintypes.DWORD()
    user.GetWindowThreadProcessId(user.GetForegroundWindow(), ctypes.byref(pid))
    return pid.value


def focus_owned(process, timeout=15):
    """Focus only this live Popen child; raise rather than accept background timings.

    Call at launch/readiness, never in a continuous focus-stealing loop. During
    measurement use foreground_pid() to reject samples if the user tabs away.
    """
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsIconic.argtypes = [wintypes.HWND]
    user.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user.BringWindowToTop.argtypes = [wintypes.HWND]
    user.SetForegroundWindow.argtypes = [wintypes.HWND]
    user.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    kernel.GetCurrentThreadId.restype = wintypes.DWORD
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Benchmark process exited before foreground acquisition")
        windows = []

        @callback_type
        def visit(hwnd, unused):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process.pid and user.IsWindowVisible(hwnd):
                windows.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        for hwnd in windows:
            current = kernel.GetCurrentThreadId()
            foreground = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
            attached = bool(foreground and foreground != current
                            and user.AttachThreadInput(current, foreground, True))
            try:
                if user.IsIconic(hwnd):
                    user.ShowWindow(hwnd, 9)  # SW_RESTORE
                user.BringWindowToTop(hwnd)
                user.SetForegroundWindow(hwnd)
            finally:
                if attached:
                    user.AttachThreadInput(current, foreground, False)
            time.sleep(0.2)
            if process.poll() is None and foreground_pid() == process.pid:
                return {"success": True, "ownedPid": process.pid,
                        "foregroundPid": process.pid, "unixMs": int(time.time() * 1000)}
        time.sleep(0.2)
    raise RuntimeError("Windows refused benchmark focus; no timing acceptance")
