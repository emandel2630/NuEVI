"""Teensy detection, reboot-to-bootloader and flashing on Windows.

* Detection walks HID interfaces with setupapi. A running NuEVI (USB_MIDI build) exposes a
  "seremu" HID interface (VID 16C0, PID 0485, MI_01); a Teensy waiting in its HalfKay
  bootloader shows up as VID 16C0, PID 0478.
* HidD_GetAttributes().VersionNumber is the USB bcdDevice, which tells the board:
  0x0275 = Teensy 3.2 (NuEVI R1), 0x0279 = Teensy 4.0 (NuEVI R2).
* Reboot: Teensyduino's seremu interface reboots into the bootloader on a 4-byte
  HID SET_REPORT(feature) of A9 45 C2 6B (cores/teensy3/usb_dev.c, cores/teensy4/usb.c).
* Programming is done by PJRC's own teensy_loader_cli.exe.
"""
import ctypes
import os
import subprocess
import time
from ctypes import wintypes

BOARDS = {
    "R1": {"label": "NuEVI R1 (Teensy 3.2)", "env": "nuevi", "mcu": "TEENSY32", "bcd": (0x0273, 0x0274, 0x0275)},
    "R2": {"label": "NuEVI R2 (Teensy 4.0)", "env": "nuevi_teensy40", "mcu": "TEENSY40", "bcd": (0x0279,)},
}

_setupapi = ctypes.WinDLL("setupapi")
_hid = ctypes.WinDLL("hid")
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID), ("Flags", wintypes.DWORD),
                ("Reserved", ctypes.c_size_t)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT), ("ProductID", wintypes.USHORT),
                ("VersionNumber", wintypes.USHORT)]


_HDEVINFO = ctypes.c_void_p
_setupapi.SetupDiGetClassDevsW.restype = _HDEVINFO
_setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
_setupapi.SetupDiEnumDeviceInterfaces.argtypes = [_HDEVINFO, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD,
                                                  ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
_setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [_HDEVINFO, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                       ctypes.c_void_p, wintypes.DWORD,
                                                       ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_setupapi.SetupDiDestroyDeviceInfoList.argtypes = [_HDEVINFO]
_hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
_hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
_hid.HidD_SetFeature.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG]
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                             wintypes.DWORD, wintypes.HANDLE]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]

INVALID_HANDLE = wintypes.HANDLE(-1).value
NO_WINDOW = 0x08000000


def _hid_paths():
    guid = GUID()
    _hid.HidD_GetHidGuid(ctypes.byref(guid))
    hdev = _setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x12)  # PRESENT | DEVICEINTERFACE
    paths = []
    try:
        i = 0
        while True:
            ifd = SP_DEVICE_INTERFACE_DATA()
            ifd.cbSize = ctypes.sizeof(ifd)
            if not _setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), i, ctypes.byref(ifd)):
                break
            i += 1
            need = wintypes.DWORD()
            _setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(ifd), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value + 4)
            ctypes.c_uint32.from_buffer(buf).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if _setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(ifd), buf, need.value, None, None):
                paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
    finally:
        _setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return paths


def _open(path, access=0xC0000000):
    h = _k32.CreateFileW(path, access, 3, None, 3, 0, None)
    return None if h in (None, INVALID_HANDLE) else h


def _attrs(path):
    h = _open(path, 0)
    if not h:
        return None
    try:
        a = HIDD_ATTRIBUTES()
        a.Size = ctypes.sizeof(a)
        return a if _hid.HidD_GetAttributes(h, ctypes.byref(a)) else None
    finally:
        _k32.CloseHandle(h)


def board_from_bcd(bcd):
    for key, b in BOARDS.items():
        if bcd in b["bcd"]:
            return key
    return None


def scan():
    """Return dict: state in {'running','bootloader','none'}, board key or None, seremu path."""
    for p in _hid_paths():
        lp = p.lower()
        if "vid_16c0&pid_0478" in lp:
            a = _attrs(p)
            return {"state": "bootloader", "board": board_from_bcd(a.VersionNumber) if a else None, "path": p}
    for p in _hid_paths():
        lp = p.lower()
        if "vid_16c0&pid_0485" in lp and "mi_01" in lp:
            a = _attrs(p)
            return {"state": "running", "board": board_from_bcd(a.VersionNumber) if a else None, "path": p}
    return {"state": "none", "board": None, "path": None}


def request_bootloader():
    """Ask a running Teensyduino firmware to jump to the bootloader. Returns True if the request was accepted."""
    dev = scan()
    if dev["state"] == "bootloader":
        return True
    if dev["state"] != "running":
        return False
    h = _open(dev["path"])
    if not h:
        return False
    try:
        report = (ctypes.c_ubyte * 5)(0x00, 0xA9, 0x45, 0xC2, 0x6B)  # report id 0 + magic
        return bool(_hid.HidD_SetFeature(h, report, 5))
    finally:
        _k32.CloseHandle(h)


def wait_for(state, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if scan()["state"] == state:
            return True
        time.sleep(0.25)
    return False


def flash(loader_exe, hex_path, board_key, log=print, manual_prompt=None):
    """Reboot (if needed) and program hex_path. manual_prompt() is called if the soft reboot didn't work;
    it should tell the user to press the Teensy program button and return False to cancel."""
    mcu = BOARDS[board_key]["mcu"]
    if scan()["state"] != "bootloader":
        log("Asking the NuEVI to enter its bootloader...")
        request_bootloader()
        if not wait_for("bootloader", 8):
            log("Soft reboot didn't work.")
            if manual_prompt is None or not manual_prompt():
                raise RuntimeError("Cancelled - the NuEVI never entered bootloader mode. Nothing was changed.")
            if not wait_for("bootloader", 60):
                raise RuntimeError("Bootloader not detected. Nothing was changed. Unplug and replug the NuEVI.")
    log("Bootloader detected. Programming (do not unplug)...")
    p = subprocess.run([loader_exe, f"--mcu={mcu}", "-w", "-v", hex_path], capture_output=True, text=True,
                       timeout=120, creationflags=NO_WINDOW)
    for line in (p.stdout + p.stderr).splitlines():
        if line.strip():
            log("  " + line.strip())
    if p.returncode != 0:
        raise RuntimeError("teensy_loader_cli failed (exit %d). The Teensy is still in bootloader mode - you can "
                           "retry flashing; no data is lost." % p.returncode)
    log("Programmed. Waiting for the NuEVI to restart...")
    if wait_for("running", 15):
        log("NuEVI is running the new firmware.")
    else:
        log("Device didn't reappear yet - unplug and replug it if it stays dark.")


def hex_valid(path, board_key):
    """Light sanity check of an Intel HEX file before flashing."""
    try:
        with open(path) as f:
            lines = [l.strip() for l in f if l.strip()]
    except OSError as e:
        return False, str(e)
    if not lines or not all(l.startswith(":") for l in lines):
        return False, "Not an Intel HEX file"
    if lines[-1] != ":00000001FF":
        return False, "HEX file is truncated (no end record)"
    for l in lines:
        b = bytes.fromhex(l[1:])
        if sum(b) & 0xFF:
            return False, "HEX checksum error"
    # Teensy 4 images live at 0x60000000 (extended linear address 6000); Teensy 3 images start at 0
    t4 = any(l.startswith(":02000004600") for l in lines)
    if board_key == "R1" and t4:
        return False, "This HEX is built for Teensy 4.0 (NuEVI R2), not your R1"
    if board_key == "R2" and not t4:
        return False, "This HEX is built for Teensy 3.2 (NuEVI R1), not R2"
    return True, "ok"
