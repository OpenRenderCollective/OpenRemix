"""What generate.py's stage compiles cost and what they made, kept between builds.

A stage's optimized SPIR-V depends only on its unoptimized SPIR-V (glslang's, fast), the
tools and the optimization: the cache keys the finished stage by those, so a stage whose
source change compiles to the same SPIR-V (a comment, code under another stage's #if,
a branch switch) skips spirv-opt, the slow step. Each stage's time and the peak memory of each of
its processes are kept too, for the scheduler: the longest stages start first, and their processes
run as many at once as memory holds.

The cache lives in _work/shader-cache (REMIX_SHADER_CACHE moves it, empty turns it off),
pruned to REMIX_SHADER_CACHE_MB (default 2048) by least recent use."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

# Bump when what a cached stage holds changes.
FORMAT = 2
_lock = threading.Lock()


def directory(root):
    configured = os.environ.get('REMIX_SHADER_CACHE')
    if configured is not None:
        return Path(configured) if configured else None
    return Path(root) / '_work/shader-cache'


def tool_identity(tools):
    """The versions of the tools a stage goes through, for the key."""
    lines = []
    for tool in tools:
        try:
            # spirv-opt and spirv-val print their version to stderr.
            result = subprocess.run([tool, '--version'], capture_output=True, text=True)
            lines.append(tool + ': ' + (result.stdout + result.stderr).strip())
        except OSError:
            lines.append(tool + ': missing')
    return '\n'.join(lines)


def key(identity, recipe, spirv):
    digest = hashlib.sha256()
    digest.update(f'{FORMAT}\n{identity}\n{recipe}\n'.encode())
    digest.update(spirv)
    return digest.hexdigest()


class Cache:
    def __init__(self, root):
        self.path = directory(root)
        if self.path:
            try:
                self.path.mkdir(parents=True, exist_ok=True)
            except OSError:
                self.path = None

    def get(self, name):
        if not self.path:
            return None
        file = self.path / name[:2] / name
        try:
            data = file.read_bytes()
        except OSError:
            return None
        try:
            os.utime(file)  # Recently used: pruned last.
        except OSError:
            pass
        return data

    def put(self, name, data):
        if not self.path:
            return
        file = self.path / name[:2] / name
        try:
            file.parent.mkdir(exist_ok=True)
            temporary = file.with_name(f'{name}.{os.getpid()}.{threading.get_ident()}.tmp')
            temporary.write_bytes(data)
            os.replace(temporary, file)
        except OSError:
            pass

    def prune(self):
        if not self.path:
            return
        limit = int(os.environ.get('REMIX_SHADER_CACHE_MB', '2048')) * 2**20
        files = []
        for file in self.path.glob('??/*'):
            try:
                stat = file.stat()
            except OSError:
                continue
            files.append((stat.st_mtime, stat.st_size, file))
        total = sum(size for _, size, _ in files)
        for _, size, file in sorted(files):
            if total <= limit:
                break
            try:
                file.unlink()
                total -= size
            except OSError:
                pass


class Statistics:
    """Each stage's last compile time (s) and peak memory (bytes), per backend."""

    def __init__(self, cache, backend):
        self.file = cache.path / f'statistics-{backend}.json' if cache.path else None
        self.values = {}
        if self.file:
            try:
                self.values = json.loads(self.file.read_text())
            except (OSError, ValueError):
                self.values = {}
        self.changed = False

    def get(self, stage):
        value = self.values.get(stage)
        return (value['seconds'], value['bytes']) if value else None

    def segments(self, stage):
        """The peak memory of each of a stage's spirv-opt runs, or None."""
        value = self.values.get(stage)
        return value.get('segments') if value else None

    def largest_segments(self, count):
        """The largest peak of each spirv-opt run among stages measured with `count` runs, or None."""
        rows = [value['segments'] for value in self.values.values() if len(value.get('segments') or []) == count]
        return [max(row[i] for row in rows) for i in range(count)] if rows else None

    def heaviest(self):
        """(seconds, bytes) of the longest and the largest stage measured, or None."""
        if not self.values:
            return None
        return (max(value['seconds'] for value in self.values.values()),
                max(value['bytes'] for value in self.values.values()))

    def set(self, stage, seconds, peak, segments=()):
        with _lock:
            self.values[stage] = {'seconds': round(seconds, 2), 'bytes': int(peak),
                                  'segments': [int(size) for size in segments]}
            self.changed = True

    def save(self):
        if self.file and self.changed:
            temporary = self.file.with_name(f'{self.file.name}.{os.getpid()}.tmp')
            try:
                temporary.write_text(json.dumps(self.values, indent=1, sort_keys=True))
                os.replace(temporary, self.file)
            except OSError:
                pass


def _windows_peak(handle):
    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_uint32), ('PageFaultCount', ctypes.c_uint32),
                    ('PeakWorkingSetSize', ctypes.c_size_t), ('WorkingSetSize', ctypes.c_size_t),
                    ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                    ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                    ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t)]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    psapi = ctypes.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_uint32]
    if psapi.GetProcessMemoryInfo(int(handle), ctypes.byref(counters), counters.cb):
        return max(counters.PeakWorkingSetSize, counters.PeakPagefileUsage)
    return 0


def run(command, **arguments):
    """subprocess.run(command, check=True); returns the process's peak memory in bytes (0 unknown)."""
    process = subprocess.Popen(command, **arguments)
    if os.name == 'nt':
        process.wait()
        peak = _windows_peak(process._handle)
    elif hasattr(os, 'wait4'):
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        peak = usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)
    else:
        process.wait()
        peak = 0
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command)
    return peak


def memory():
    """(total, available) physical memory in bytes, or None."""
    if os.name == 'nt':
        class Status(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_uint32), ('dwMemoryLoad', ctypes.c_uint32),
                        ('ullTotalPhys', ctypes.c_uint64), ('ullAvailPhys', ctypes.c_uint64),
                        ('ullTotalPageFile', ctypes.c_uint64), ('ullAvailPageFile', ctypes.c_uint64),
                        ('ullTotalVirtual', ctypes.c_uint64), ('ullAvailVirtual', ctypes.c_uint64),
                        ('ullAvailExtendedVirtual', ctypes.c_uint64)]
        status = Status()
        status.dwLength = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return status.ullTotalPhys, status.ullAvailPhys
        return None
    try:
        values = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
        return (int(values['MemTotal'].split()[0]) * 1024, int(values['MemAvailable'].split()[0]) * 1024)
    except (OSError, KeyError, ValueError):
        pass
    try:
        total = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
        return total, total
    except (ValueError, OSError, AttributeError):
        return None




class Pool:
    """The memory the processes running at once may take: a process reserves its estimated peak
    and waits, first come first served, until it fits in most of the machine's memory and in what
    is free now. One runs alone whatever its estimate."""

    def __init__(self):
        machine = memory()
        self.budget = machine[0] * 0.8 if machine else float('inf')
        # Free memory a start leaves for everything else.
        self.reserve = 2 * 2**30
        self.used = 0
        self.holders = 0
        self.queue = []
        self.condition = threading.Condition()

    def _fits(self, size):
        if self.holders == 0:
            return True
        if self.used + size > self.budget:
            return False
        now = memory()
        return now is None or size + self.reserve <= now[1]

    def acquire(self, size):
        ticket = object()
        with self.condition:
            self.queue.append(ticket)
            while self.queue[0] is not ticket or not self._fits(size):
                self.condition.wait(0.5)
            self.queue.pop(0)
            self.used += size
            self.holders += 1
            self.condition.notify_all()

    def release(self, size):
        with self.condition:
            self.used -= size
            self.holders -= 1
            self.condition.notify_all()

    def run(self, size, command, **arguments):
        """run(command) within a reservation of `size` bytes; returns its peak memory."""
        self.acquire(size)
        try:
            return run(command, **arguments)
        finally:
            self.release(size)


def schedule(tasks, workers):
    """Runs tasks [(name, function, seconds)], the longest first, at most `workers` at once (their
    processes share a Pool for memory). Returns {name: result}; the first failure is raised."""
    pending = sorted(tasks, key=lambda task: -task[2])
    results, failures = {}, []
    lock = threading.Lock()

    def worker():
        while True:
            with lock:
                if failures or not pending:
                    return
                task = pending.pop(0)
            try:
                result = task[1]()
            except BaseException as error:
                with lock:
                    failures.append(error)
            else:
                with lock:
                    results[task[0]] = result

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, min(workers, len(pending))))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if failures:
        raise failures[0]
    return results
