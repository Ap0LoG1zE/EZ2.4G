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
        dlen = struct.unpack_from("<I", pkt, hlen - 4)[0]
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


def main():
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
                    creationflags=CREATE_NO_WINDOW)
            except Exception:
                continue
            time.sleep(1.5)
            try:
                p.terminate()
                p.wait(2)
            except Exception:
                pass
            devs = scan_devices(probe)
            if (vid, pid) in devs:
                found = hub
                break
        if not found:
            status("notfound")
            return
    else:
        found = hubs[0] if hubs else 1

    status("capturing", found)
    final = os.path.join(outdir, "capture.pcap")
    try:
        p = subprocess.Popen(
            [cp, "-d", "\\\\.\\USBPcap%d" % found, "-A",
             "--inject-descriptors", "-o", final],
            creationflags=CREATE_NO_WINDOW)
    except Exception:
        status("error")
        return
    while not os.path.exists(stop_flag):
        if p.poll() is not None:
            break
        time.sleep(0.2)
    try:
        p.terminate()
        p.wait(3)
    except Exception:
        pass
    status("done", found, final)


if __name__ == "__main__":
    main()
