"""NuEVI / NuRAD config-management sysex protocol over Windows winmm (no third-party deps).

Protocol (see NuEVI/settings.cpp):
  host -> device   F0 00 3E 7F "NuEVIc00" F7          request config dump
  host -> device   F0 00 3E 7F "NuEVIc03" F7          request version
  device -> host   F0 00 3E 7F "NuEVIc04" ee ee fw.. F7   eeprom version (14-bit) + fw string
  device -> host   F0 00 3E 7F "NuEVIc01" len(2) payload crc(4) F7   config dump
  host -> device   the same c01 message                restore config

The device only answers while in config-management mode (power on holding UP+DOWN,
or on the About screen press UP+DOWN).
"""
import ctypes
import json
import time
from ctypes import wintypes

VENDOR = bytes([0x00, 0x3E, 0x7F])
HEADER = VENDOR + b"NuEVI"

# ---------------------------------------------------------------- winmm binding
_w = ctypes.WinDLL("winmm")

MHDR_DONE = 0x1
CALLBACK_NULL = 0
MMSYSERR_NOERROR = 0


class MIDIHDR(ctypes.Structure):
    pass


MIDIHDR._fields_ = [
    ("lpData", ctypes.c_void_p),
    ("dwBufferLength", wintypes.DWORD),
    ("dwBytesRecorded", wintypes.DWORD),
    ("dwUser", ctypes.c_size_t),
    ("dwFlags", wintypes.DWORD),
    ("lpNext", ctypes.POINTER(MIDIHDR)),
    ("reserved", ctypes.c_size_t),
    ("dwOffset", wintypes.DWORD),
    ("dwReserved", ctypes.c_size_t * 8),
]


class MIDIINCAPSW(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32), ("dwSupport", wintypes.DWORD)]


class MIDIOUTCAPSW(ctypes.Structure):
    _fields_ = [("wMid", wintypes.WORD), ("wPid", wintypes.WORD), ("vDriverVersion", wintypes.UINT),
                ("szPname", wintypes.WCHAR * 32), ("wTechnology", wintypes.WORD), ("wVoices", wintypes.WORD),
                ("wNotes", wintypes.WORD), ("wChannelMask", wintypes.WORD), ("dwSupport", wintypes.DWORD)]


_H = ctypes.c_void_p
_w.midiInOpen.argtypes = [ctypes.POINTER(_H), wintypes.UINT, ctypes.c_size_t, ctypes.c_size_t, wintypes.DWORD]
_w.midiOutOpen.argtypes = [ctypes.POINTER(_H), wintypes.UINT, ctypes.c_size_t, ctypes.c_size_t, wintypes.DWORD]
for _name in ("midiInPrepareHeader", "midiInUnprepareHeader", "midiInAddBuffer",
              "midiOutPrepareHeader", "midiOutUnprepareHeader", "midiOutLongMsg"):
    getattr(_w, _name).argtypes = [_H, ctypes.POINTER(MIDIHDR), wintypes.UINT]
for _name in ("midiInStart", "midiInStop", "midiInReset", "midiInClose", "midiOutReset", "midiOutClose"):
    getattr(_w, _name).argtypes = [_H]
_w.midiInGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(MIDIINCAPSW), wintypes.UINT]
_w.midiOutGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(MIDIOUTCAPSW), wintypes.UINT]


class MidiError(Exception):
    pass


def _check(rc, what):
    if rc != MMSYSERR_NOERROR:
        hint = " (is another program, e.g. a DAW, using the NuEVI MIDI port?)" if rc == 4 else ""
        raise MidiError(f"{what} failed, winmm error {rc}{hint}")


def list_ports():
    ins, outs = [], []
    for i in range(_w.midiInGetNumDevs()):
        c = MIDIINCAPSW()
        if _w.midiInGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            ins.append((i, c.szPname))
    for i in range(_w.midiOutGetNumDevs()):
        c = MIDIOUTCAPSW()
        if _w.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c)) == 0:
            outs.append((i, c.szPname))
    return ins, outs


def find_device():
    """Return (in_id, out_id, name) of the first NuEVI/NuRAD port pair, or None."""
    ins, outs = list_ports()
    for want in ("NuEVI", "NuRAD"):
        i = next((i for i, n in ins if want in n), None)
        o = next((o for o, n in outs if want in n), None)
        if i is not None and o is not None:
            return i, o, next(n for _, n in ins if want in n)
    return None


class Connection:
    """Open NuEVI MIDI in/out ports and exchange sysex messages."""

    NBUF = 4
    BUFSIZE = 8192

    def __init__(self, in_id=None, out_id=None):
        if in_id is None or out_id is None:
            dev = find_device()
            if not dev:
                raise MidiError("No NuEVI/NuRAD MIDI device found. Is it plugged in?")
            in_id, out_id, _ = dev
        self.hin, self.hout = _H(), _H()
        _check(_w.midiInOpen(ctypes.byref(self.hin), in_id, 0, 0, CALLBACK_NULL), "Opening MIDI input")
        try:
            _check(_w.midiOutOpen(ctypes.byref(self.hout), out_id, 0, 0, CALLBACK_NULL), "Opening MIDI output")
        except MidiError:
            _w.midiInClose(self.hin)
            raise
        self._bufs, self._hdrs = [], []
        for _ in range(self.NBUF):
            buf = ctypes.create_string_buffer(self.BUFSIZE)
            h = MIDIHDR()
            h.lpData = ctypes.cast(buf, ctypes.c_void_p)
            h.dwBufferLength = self.BUFSIZE
            _check(_w.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h)), "Preparing input buffer")
            _check(_w.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h)), "Queueing input buffer")
            self._bufs.append(buf)
            self._hdrs.append(h)
        self._partial = b""
        _check(_w.midiInStart(self.hin), "Starting MIDI input")

    def close(self):
        if self.hin:
            _w.midiInStop(self.hin)
            _w.midiInReset(self.hin)  # returns all buffers
            for h in self._hdrs:
                _w.midiInUnprepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            _w.midiInClose(self.hin)
            self.hin = None
        if self.hout:
            _w.midiOutReset(self.hout)
            _w.midiOutClose(self.hout)
            self.hout = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def send(self, msg: bytes):
        buf = ctypes.create_string_buffer(msg, len(msg))
        h = MIDIHDR()
        h.lpData = ctypes.cast(buf, ctypes.c_void_p)
        h.dwBufferLength = len(msg)
        _check(_w.midiOutPrepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h)), "Preparing output")
        try:
            _check(_w.midiOutLongMsg(self.hout, ctypes.byref(h), ctypes.sizeof(h)), "Sending sysex")
            deadline = time.time() + 5
            while not (h.dwFlags & MHDR_DONE):
                if time.time() > deadline:
                    raise MidiError("Timed out sending sysex")
                time.sleep(0.005)
        finally:
            _w.midiOutUnprepareHeader(self.hout, ctypes.byref(h), ctypes.sizeof(h))

    def _poll(self):
        """Collect completed sysex messages from input buffers."""
        out = []
        for buf, h in zip(self._bufs, self._hdrs):
            if h.dwFlags & MHDR_DONE:
                data = ctypes.string_at(buf, h.dwBytesRecorded)
                h.dwBytesRecorded = 0
                h.dwFlags &= ~MHDR_DONE
                _w.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
                self._partial += data
                while b"\xf7" in self._partial:
                    msg, _, self._partial = self._partial.partition(b"\xf7")
                    start = msg.rfind(b"\xf0")
                    if start >= 0:
                        out.append(msg[start:] + b"\xf7")
        return out

    def receive(self, code: bytes, timeout=3.0):
        """Wait for a NuEVI sysex with the given 3-byte code (e.g. b'c01')."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for m in self._poll():
                if m[1:9] == HEADER and m[9:12] == code:
                    return m
            time.sleep(0.01)
        return None

    def request(self, code: bytes, reply: bytes, timeout=3.0):
        self._poll()  # drop anything stale
        self.send(b"\xf0" + HEADER + code + b"\xf7")
        return self.receive(reply, timeout)


# ---------------------------------------------------------------- message helpers
def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return (~crc) & 0xFFFFFFFF


def _midi14(b: bytes, pos: int) -> int:
    return (b[pos] & 0x7F) << 7 | (b[pos + 1] & 0x7F)


def _to_midi14(v: int) -> bytes:
    return bytes([(v >> 7) & 0x7F, v & 0x7F])


def _midi_crc(crc: int) -> bytes:
    # firmware: r = p0<<24|p1<<16|p2<<8|p3 (each &0x7F), stored little-endian -> wire p3 p2 p1 p0
    p = crc.to_bytes(4, "little")
    return bytes([p[3] & 0x7F, p[2] & 0x7F, p[1] & 0x7F, p[0] & 0x7F])


def parse_version(msg: bytes):
    body = msg[1:-1]
    return {"eeprom_version": _midi14(body, 11), "firmware": body[13:].decode("ascii", "replace")}


def parse_dump(msg: bytes):
    """Validate a c01/c02 dump (with F0/F7). Returns dict with values list. Raises ValueError."""
    if msg[:1] != b"\xf0" or msg[-1:] != b"\xf7":
        raise ValueError("Not a sysex message")
    body = msg[1:-1]
    if body[:8] != HEADER or body[8:11] not in (b"c01", b"c02"):
        raise ValueError("Not a NuEVI config dump")
    size = _midi14(body, 11)
    payload = body[13:13 + size]
    crc_rcv = body[13 + size:17 + size]
    if len(payload) != size or len(crc_rcv) != 4:
        raise ValueError("Truncated config dump")
    if crc_rcv != bytes([0x7F, 0x00, 0x7F, 0x00]) and crc_rcv != _midi_crc(crc32(body[:13 + size])):
        raise ValueError("Checksum mismatch - dump is corrupt")
    values = [_midi14(payload, i) for i in range(0, size, 2)]
    return {"eeprom_size": size, "eeprom_version": values[0], "values": values}


def build_dump(values) -> bytes:
    payload = b"".join(_to_midi14(v) for v in values)
    body = HEADER + b"c01" + _to_midi14(len(payload)) + payload
    return b"\xf0" + body + _midi_crc(crc32(body)) + b"\xf7"


NO_ANSWER_HELP = ("The NuEVI did not answer.\n\n"
                  "- If its screen does not say \"Config mgmt\": power it on while holding UP+DOWN.\n"
                  "- If it says \"Config mgmt\" and shows \"Sending...\" but this still fails, its USB MIDI "
                  "replies are getting stuck: unplug it and power it on again holding UP+DOWN, then retry.")


def backup(conn: Connection):
    ver = None
    for _ in range(2):  # the first request right after entering config mode is sometimes lost
        ver = conn.request(b"c03", b"c04")
        if ver is not None:
            break
    if ver is None:
        raise MidiError(NO_ANSWER_HELP)
    info = parse_version(ver)
    dump = conn.request(b"c00", b"c01", timeout=5)
    if dump is None:
        raise MidiError("Device answered version request but sent no config dump")
    parsed = parse_dump(dump)
    return info, dump, parsed


def restore(conn: Connection, syx: bytes, verify=True):
    """Send a dump back to the device, then re-read and compare. Returns (ok, message)."""
    parsed = parse_dump(syx)
    ver = conn.request(b"c03", b"c04")
    if ver is None:
        ver = conn.request(b"c03", b"c04")
    if ver is None:
        raise MidiError(NO_ANSWER_HELP)
    info = parse_version(ver)
    if info["eeprom_version"] != parsed["eeprom_version"]:
        return False, (f"Backup is settings version {parsed['eeprom_version']} but the firmware on the device "
                       f"uses version {info['eeprom_version']}. Flash the matching firmware first.")
    conn.send(syx)
    time.sleep(1.0)
    if not verify:
        return True, "Sent."
    _, _, now = backup(conn)
    # calibration values may be skipped by firmware when out of range; compare everything else
    diffs = [i * 2 for i, (a, b) in enumerate(zip(now["values"], parsed["values"])) if a != b]
    if diffs:
        return False, f"Restore sent but {len(diffs)} values differ afterwards (addresses {diffs[:10]})"
    return True, "Restored and verified."


def save_backup(path_base: str, info, dump: bytes, parsed):
    with open(path_base + ".syx", "wb") as f:
        f.write(dump)
    with open(path_base + ".json", "w") as f:
        json.dump({"device": info, "saved": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "eeprom_size": parsed["eeprom_size"], "values": parsed["values"]}, f, indent=1)


if __name__ == "__main__":
    import os
    import sys
    with Connection() as c:
        info, dump, parsed = backup(c)
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser("~"), "Documents", "NuEVI Backups")
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, time.strftime("settings_%Y-%m-%d_%H%M%S") + f"_fw{info['firmware']}_v{info['eeprom_version']}")
    save_backup(base, info, dump, parsed)
    print(f"Firmware {info['firmware']}, settings version {info['eeprom_version']}, {parsed['eeprom_size']} bytes")
    print("Saved", base + ".syx")
