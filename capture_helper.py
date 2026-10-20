import ctypes
import ctypes.wintypes as w
import json
import os
import struct
import subprocess
import sys
import time

CREATE_NO_WINDOW = 0x08000000

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = w.HANDLE
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                            w.DWORD, w.DWORD, w.HANDLE]


def scan_devices(path):
    out = []
    try:
        with open(path, "rb") as f:
            data = f.read()
    except Exception:
        return out
    if len(data) < 24:
        return out
    off = 24
    n = len(data)
    while off + 16 <= n:
        try:
            _ts, _tu, incl, _orig = struct.unpack_from("<IIII", data, off)
        except Exception:
            break
        off += 16
        if incl <= 0 or off + incl > n:
            break
        pkt = data[off:off + incl]
        off += incl
        if len(pkt) < 31:
            continue
        hlen = struct.unpack_from("<H", pkt, 0)[0]
        if not (27 <= hlen <= 600) or len(pkt) < hlen:
            continue
        dlen = struct.unpack_from("<I", pkt, 23)[0]
        payload = pkt[hlen:hlen + dlen]
        for i in range(0, len(payload) - 17):
            if payload[i] == 18 and payload[i + 1] == 1:
                vid = payload[i + 8] | (payload[i + 9] << 8)
                pid = payload[i + 10] | (payload[i + 11] << 8)
                if vid and pid and (vid, pid) not in out:
                    out.append((vid, pid))
    return out


def hub_exists(n):
    h = k32.CreateFileW("\\\\.\\USBPcap%d" % n, 0, 3, None, 3, 0, None)
    err = ctypes.get_last_error()
    if h and h != w.HANDLE(-1).value:
        k32.CloseHandle(h)
        return True
    return err == 5


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [("ReadOperationCount", ctypes.c_ulonglong),
                ("WriteOperationCount", ctypes.c_ulonglong),
                ("OtherOperationCount", ctypes.c_ulonglong),
                ("ReadTransferCount", ctypes.c_ulonglong),
                ("WriteTransferCount", ctypes.c_ulonglong),
                ("OtherTransferCount", ctypes.c_ulonglong)]


class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", w.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", w.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", w.DWORD),
                ("SchedulingClass", w.DWORD)]


class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation",
                 _JOBOBJECT_BASIC_LIMIT_INFORMATION),
                ("IoInfo", _IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


_JOB = None


def _setup_job():
    """把自己放进 kill-on-close 的 Job，进程退出时连带收掉 USBPcapCMD。"""
    global _JOB
    try:
        k32.CreateJobObjectW.restype = w.HANDLE
        k32.GetCurrentProcess.restype = w.HANDLE
        k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
        job = k32.CreateJobObjectW(None, None)
        if not job:
            return
        info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        info.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
        k32.SetInformationJobObject(job, 9, ctypes.byref(info),
                                    ctypes.sizeof(info))
        k32.AssignProcessToJobObject(job, k32.GetCurrentProcess())
        _JOB = job
    except Exception:
        _JOB = None


def main():
    _setup_job()
    (cp, vid_s, pid_s, outdir, stop_flag, status_file) = sys.argv[1:7]
    vid = int(vid_s, 16)
    pid = int(pid_s, 16)

    def status(state, hub=0, file=""):
        tmp = status_file + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"state": state, "hub": hub, "file": file}, f)
            os.replace(tmp, status_file)
        except Exception:
            pass

    hubs = [n for n in range(1, 7) if hub_exists(n)]
    status("probing")
    any_bytes = False
    found = 0
    if vid:
        for hub in hubs:
            probe = os.path.join(outdir, "probe%d.pcap" % hub)
            try:
                os.remove(probe)
            except Exception:
                pass
            try:
                p = subprocess.Popen(
                    [cp, "-d", "\\\\.\\USBPcap%d" % hub, "-A",
                     "--inject-descriptors", "-o", probe],
                    creationflags=CREATE_NO_WINDOW,
                    stderr=subprocess.DEVNULL)
            except Exception:
                continue
            time.sleep(4.0)
            try:
                p.terminate()
                p.wait(3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
            try:
                if os.path.getsize(probe) > 0:
                    any_bytes = True
            except Exception:
                pass
            devs = scan_devices(probe)
            if (vid, pid) in devs:
                found = hub
                break
        if not found:
            status("notraffic" if any_bytes else "busy")
            return
    else:
        found = hubs[0] if hubs else 1

    status("capturing", found)
    final = os.path.join(outdir, "capture.pcap")
    try:
        p = subprocess.Popen(
            [cp, "-d", "\\\\.\\USBPcap%d" % found, "-A",
             "--inject-descriptors", "-o", final],
            creationflags=CREATE_NO_WINDOW,
            stderr=subprocess.DEVNULL)
    except Exception:
        status("error")
        return
    started = time.time()
    while not os.path.exists(stop_flag):
        if p.poll() is not None:
            break
        if time.time() - started > 180:
            break
        try:
            if os.path.getsize(final) > 256 * 1024 * 1024:
                break
        except Exception:
            pass
        time.sleep(0.2)
    try:
        p.terminate()
        p.wait(3)
    except Exception:
        try:
            p.kill()
            p.wait(2)
        except Exception:
            pass
    status("done", found, final)


if __name__ == "__main__":
    main()
