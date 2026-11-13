"""NuEVI Update Tool - back up / restore settings, flash or roll back firmware, build from source.

Runs on Windows (Python 3 with tkinter). Launched from the desktop shortcut; the source lives in the
NuEVI repo (configtool/) so edits there take effect the next time the tool is opened.
"""
import datetime
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import traceback
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nuevi_sysex as sx  # noqa: E402
import teensy_flash as tf  # noqa: E402

NO_WINDOW = 0x08000000
HOME = os.path.expanduser("~")
APPDIR = os.path.join(os.environ.get("LOCALAPPDATA", HOME), "NuEVIUpdateTool")
LIBDIR = os.path.join(HOME, "Documents", "NuEVI Backups")
FWDIR = os.path.join(LIBDIR, "firmware")
LOADER = os.path.join(APPDIR, "teensy", "teensy_loader_cli.exe")
CONFIG = os.path.join(APPDIR, "config.json")
CONFIG_MODE_HELP = ("Put the NuEVI in CONFIG MODE:\n\n"
                    "Turn it off (unplug USB), hold the UP and DOWN buttons, and plug it back in while holding them. "
                    "The screen shows \"Config mgmt\".\n\n"
                    "Close any DAW/MIDI program that is using the NuEVI.")


def repo_from_script():
    """Work out the WSL distro and Linux repo path from a \\\\wsl.localhost\\<distro>\\... script path."""
    here = os.path.dirname(os.path.abspath(__file__))
    m = re.match(r"^\\\\wsl(?:\.localhost|\$)\\([^\\]+)(\\.*)$", here, re.I)
    if not m:
        return None, None
    distro, rest = m.group(1), m.group(2).replace("\\", "/")
    return distro, rest.rsplit("/configtool", 1)[0]


def load_config():
    cfg = {}
    try:
        with open(CONFIG) as f:
            cfg = json.load(f)
    except (OSError, ValueError):
        pass
    distro, repo = repo_from_script()
    cfg.setdefault("distro", distro or "Ubuntu")
    cfg.setdefault("repo", repo or "/home/" + os.environ.get("USERNAME", "") + "/NuEVI")
    cfg.setdefault("board", "R1")
    return cfg


def save_config(cfg):
    os.makedirs(APPDIR, exist_ok=True)
    with open(CONFIG, "w") as f:
        json.dump(cfg, f, indent=1)


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.cfg = load_config()
        self.title("NuEVI Update Tool")
        self.geometry("900x680")
        self.minsize(760, 560)
        self.q = queue.Queue()
        self.busy = False
        self.device = {"state": "none", "board": None}
        self.device_info = None  # last version answer from config mode
        self.backed_up_this_session = False
        os.makedirs(FWDIR, exist_ok=True)

        self._build_ui()
        self.refresh_backups()
        self.refresh_firmware()
        self.after(100, self._pump)
        self.after(300, self._poll_device)
        self.after(500, lambda: self.run_task(self._task_git_status, quiet=True))
        if not os.path.exists(LOADER):
            self.log("WARNING: teensy_loader_cli.exe not found at " + LOADER + " - flashing is disabled.")

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        style = ttk.Style(self)
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass
        style.configure("Status.TLabel", font=("Segoe UI", 11, "bold"))

        top = ttk.Frame(self, padding=(10, 8))
        top.pack(fill="x")
        self.status_var = tk.StringVar(value="Looking for NuEVI...")
        ttk.Label(top, textvariable=self.status_var, style="Status.TLabel").pack(side="left")
        ttk.Label(top, text="   Board:").pack(side="left")
        self.board_var = tk.StringVar(value=tf.BOARDS[self.cfg["board"]]["label"])
        self.board_box = ttk.Combobox(top, state="readonly", width=24, textvariable=self.board_var,
                                      values=[b["label"] for b in tf.BOARDS.values()])
        self.board_box.pack(side="left")
        self.board_box.bind("<<ComboboxSelected>>", self._board_changed)
        ttk.Button(top, text="Read device info", command=self.read_info).pack(side="right")

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=10)
        nb.add(self._settings_tab(nb), text="  Settings  ")
        nb.add(self._firmware_tab(nb), text="  Firmware  ")
        nb.add(self._develop_tab(nb), text="  Develop  ")

        logf = ttk.LabelFrame(self, text="Log", padding=4)
        logf.pack(fill="both", expand=False, padx=10, pady=(6, 10))
        self.logbox = ScrolledText(logf, height=10, font=("Consolas", 9), state="disabled", wrap="word")
        self.logbox.pack(fill="both", expand=True)

    def _tree(self, parent, cols):
        frame = ttk.Frame(parent)
        tree = ttk.Treeview(frame, columns=[c for c, _ in cols], show="headings", selectmode="browse", height=9)
        for c, w in cols:
            tree.heading(c, text=c)
            tree.column(c, width=w, anchor="w")
        sb = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
        tree.configure(yscrollcommand=sb.set)
        tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        return frame, tree

    def _settings_tab(self, nb):
        f = ttk.Frame(nb, padding=10)
        ttk.Label(f, wraplength=840, justify="left",
                  text="Settings backups are complete copies of the NuEVI's memory (all menu settings and sensor "
                       "calibration). A backup can only be restored onto firmware with the same settings version.").pack(anchor="w")
        frame, self.backup_tree = self._tree(f, [("Saved", 150), ("Firmware", 90), ("Settings ver", 90), ("File", 440)])
        frame.pack(fill="both", expand=True, pady=8)
        bar = ttk.Frame(f)
        bar.pack(fill="x")
        ttk.Button(bar, text="Back up settings now", command=self.backup_now).pack(side="left")
        ttk.Button(bar, text="Restore selected backup", command=self.restore_selected).pack(side="left", padx=6)
        ttk.Button(bar, text="Open backups folder", command=lambda: os.startfile(LIBDIR)).pack(side="right")
        return f

    def _firmware_tab(self, nb):
        f = ttk.Frame(nb, padding=10)
        ttk.Label(f, wraplength=840, justify="left",
                  text="Firmware library. Flashing keeps your settings (new firmware upgrades them automatically). "
                       "To go back, flash an older firmware, then restore the settings backup made with it.").pack(anchor="w")
        frame, self.fw_tree = self._tree(f, [("Name", 300), ("Firmware", 70), ("Settings ver", 85), ("Board", 55),
                                             ("Source", 180), ("Added", 120)])
        frame.pack(fill="both", expand=True, pady=8)
        bar = ttk.Frame(f)
        bar.pack(fill="x")
        ttk.Button(bar, text="Flash selected", command=self.flash_selected).pack(side="left")
        ttk.Button(bar, text="Roll back to original firmware", command=self.rollback).pack(side="left", padx=6)
        ttk.Button(bar, text="Import .hex...", command=self.import_hex).pack(side="left")
        ttk.Button(bar, text="Open firmware folder", command=lambda: os.startfile(FWDIR)).pack(side="right")
        return f

    def _develop_tab(self, nb):
        f = ttk.Frame(nb, padding=10)
        self.git_var = tk.StringVar(value="Source: checking...")
        ttk.Label(f, textvariable=self.git_var, wraplength=840, justify="left").pack(anchor="w")
        bar = ttk.Frame(f)
        bar.pack(fill="x", pady=8)
        ttk.Button(bar, text="Build firmware from source", command=lambda: self.run_task(self._task_build)).pack(side="left")
        ttk.Button(bar, text="Build && flash", command=self.build_and_flash).pack(side="left", padx=6)
        ttk.Button(bar, text="Run chord tests", command=lambda: self.run_task(self._task_tests)).pack(side="left")
        ttk.Button(bar, text="Refresh", command=lambda: self.run_task(self._task_git_status, quiet=True)).pack(side="right")
        bar2 = ttk.Frame(f)
        bar2.pack(fill="x")
        ttk.Button(bar2, text="Open NuEVI source in VS Code", command=lambda: self._vscode(self.cfg["repo"])).pack(side="left")
        ttk.Button(bar2, text="Edit this tool in VS Code",
                   command=lambda: self._vscode(self.cfg["repo"] + "/configtool")).pack(side="left", padx=6)
        ttk.Button(bar2, text="Open source folder", command=lambda: os.startfile(self._unc(self.cfg["repo"]))).pack(side="left")
        ttk.Label(f, wraplength=840, justify="left", foreground="#555",
                  text="\nWorkflow: edit the code (or ask Claude Code to), click \"Build && flash\". Each build is "
                       "saved in the firmware library with its git commit, so you can always flash an earlier one. "
                       "Builds run in WSL (" + self.cfg["distro"] + ") with PlatformIO.").pack(anchor="w")
        return f

    # ------------------------------------------------------------------ plumbing
    def log(self, msg):
        self.q.put(("log", msg))

    def ui(self, fn, *a):
        """Run fn on the UI thread from a worker and wait for its result."""
        ev, box = threading.Event(), {}

        def call():
            try:
                box["r"] = fn(*a)
            finally:
                ev.set()
        self.q.put(("call", call))
        ev.wait()
        return box.get("r")

    def _pump(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "log":
                    stamp = time.strftime("%H:%M:%S ")
                    self.logbox.configure(state="normal")
                    self.logbox.insert("end", stamp + val + "\n")
                    self.logbox.see("end")
                    self.logbox.configure(state="disabled")
                elif kind == "call":
                    val()
        except queue.Empty:
            pass
        self.after(80, self._pump)

    def run_task(self, fn, *args, quiet=False):
        if self.busy:
            messagebox.showinfo("Busy", "Another operation is still running.")
            return
        self.busy = True
        self.config(cursor="watch")

        def worker():
            try:
                fn(*args)
            except Exception as e:  # report every failure in the UI
                self.log("ERROR: " + str(e))
                if not quiet:
                    self.ui(messagebox.showerror, "NuEVI Update Tool", str(e))
                if not isinstance(e, (sx.MidiError, RuntimeError, ValueError)):
                    self.log(traceback.format_exc())
            finally:
                self.busy = False
                self.q.put(("call", lambda: self.config(cursor="")))
        threading.Thread(target=worker, daemon=True).start()

    def _poll_device(self):
        if not self.busy:
            try:
                d = tf.scan()
            except Exception:
                d = {"state": "none", "board": None}
            if d["state"] != self.device["state"]:
                self.device_info = None if d["state"] != "running" else self.device_info
            self.device = d
            if d["board"] and d["board"] != self.cfg["board"]:
                self.cfg["board"] = d["board"]
                self.board_var.set(tf.BOARDS[d["board"]]["label"])
                save_config(self.cfg)
            text = {"running": "NuEVI connected", "bootloader": "NuEVI in bootloader (ready to flash)",
                    "none": "No NuEVI connected"}[d["state"]]
            if d["state"] == "running" and self.device_info:
                text += f"  -  firmware {self.device_info['firmware']}, settings v{self.device_info['eeprom_version']}"
            self.status_var.set(text)
        self.after(1500, self._poll_device)

    def _board_changed(self, _e=None):
        label = self.board_var.get()
        self.cfg["board"] = next(k for k, b in tf.BOARDS.items() if b["label"] == label)
        save_config(self.cfg)

    # ------------------------------------------------------------------ lists
    def refresh_backups(self):
        self.backup_tree.delete(*self.backup_tree.get_children())
        rows = []
        for name in os.listdir(LIBDIR):
            if not name.lower().endswith(".syx"):
                continue
            path = os.path.join(LIBDIR, name)
            meta = read_json(path[:-4] + ".json")
            fw = meta.get("device", {}).get("firmware", "?")
            ver = meta.get("device", {}).get("eeprom_version")
            if ver is None:
                try:
                    with open(path, "rb") as fh:
                        ver = sx.parse_dump(fh.read())["eeprom_version"]
                except (OSError, ValueError):
                    ver = "?"
            saved = meta.get("saved") or datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime("%Y-%m-%d %H:%M:%S")
            rows.append((saved, fw, ver, name))
        for r in sorted(rows, reverse=True):
            self.backup_tree.insert("", "end", iid=r[3], values=r)

    def refresh_firmware(self):
        self.fw_tree.delete(*self.fw_tree.get_children())
        rows = []
        for name in os.listdir(FWDIR):
            if not name.lower().endswith(".hex"):
                continue
            meta = read_json(os.path.join(FWDIR, name[:-4] + ".json"))
            added = meta.get("built") or datetime.datetime.fromtimestamp(
                os.path.getmtime(os.path.join(FWDIR, name))).strftime("%Y-%m-%d %H:%M")
            label = meta.get("label", "")
            rows.append((added, (name if not label else f"{label}  ({name})", meta.get("firmware", "?"),
                                 meta.get("settings_version", "?"), meta.get("board", "?"),
                                 meta.get("source", ""), added), name))
        for _, vals, name in sorted(rows, reverse=True):
            self.fw_tree.insert("", "end", iid=name, values=vals)

    # ------------------------------------------------------------------ device actions
    def _need_config_mode(self, why):
        return messagebox.askokcancel("Config mode needed", f"{why}\n\n{CONFIG_MODE_HELP}\n\nClick OK when the screen "
                                      "shows \"Config mgmt\".")

    def read_info(self):
        def task():
            with sx.Connection() as c:
                ver = c.request(b"c03", b"c04") or c.request(b"c03", b"c04")
            if ver is None:
                raise sx.MidiError(sx.NO_ANSWER_HELP)
            self.device_info = sx.parse_version(ver)
            self.log(f"Device: firmware {self.device_info['firmware']}, settings version {self.device_info['eeprom_version']}")
        self.run_task(task)

    def _do_backup(self):
        with sx.Connection() as c:
            info, dump, parsed = sx.backup(c)
        self.device_info = info
        base = os.path.join(LIBDIR, time.strftime("settings_%Y-%m-%d_%H%M%S") +
                            f"_fw{info['firmware']}_v{info['eeprom_version']}")
        sx.save_backup(base, info, dump, parsed)
        self.backed_up_this_session = True
        self.log(f"Backed up settings (firmware {info['firmware']}, settings v{info['eeprom_version']}, checksum OK) "
                 f"-> {os.path.basename(base)}.syx")
        self.q.put(("call", self.refresh_backups))
        return info

    def backup_now(self):
        if self._need_config_mode("Backing up reads the settings from the NuEVI."):
            self.run_task(self._do_backup)

    def restore_selected(self):
        sel = self.backup_tree.selection()
        if not sel:
            messagebox.showinfo("Restore", "Select a backup first.")
            return
        path = os.path.join(LIBDIR, sel[0])
        with open(path, "rb") as fh:
            data = fh.read()
        try:
            parsed = sx.parse_dump(data)
        except ValueError as e:
            messagebox.showerror("Restore", f"That backup file is not usable: {e}")
            return
        if not messagebox.askyesno("Restore settings", f"Overwrite ALL settings on the NuEVI with\n{sel[0]}?\n\n"
                                   f"(settings version {parsed['eeprom_version']} - the NuEVI must be running "
                                   "firmware with the same settings version)"):
            return
        if not self._need_config_mode("Restoring writes the settings to the NuEVI."):
            return

        def task():
            with sx.Connection() as c:
                ok, msg = sx.restore(c, data)
            self.log(("Restore: " if ok else "Restore FAILED: ") + msg)
            if ok:
                self.ui(messagebox.showinfo, "Restore", msg + "\n\nPower the NuEVI off and on to leave config mode.")
            else:
                self.ui(messagebox.showerror, "Restore", msg)
        self.run_task(task)

    # ------------------------------------------------------------------ firmware actions
    def _ensure_backup(self):
        """Returns False to cancel. Called on the UI thread."""
        if self.backed_up_this_session:
            return True
        ans = messagebox.askyesnocancel(
            "Back up first?", "Back up the NuEVI's current settings before flashing?\n\n"
            "Yes - back up now (recommended)\nNo - flash without a new backup\nCancel - do nothing")
        if ans is None:
            return False
        if ans is False:
            return True
        if not self._need_config_mode("Backing up reads the settings from the NuEVI."):
            return False
        return "backup"

    def _flash_task(self, hex_path, do_backup):
        if do_backup:
            self._do_backup()
        meta = read_json(hex_path[:-4] + ".json")
        board = self.cfg["board"]
        self.log(f"Flashing {os.path.basename(hex_path)} to {tf.BOARDS[board]['label']}")

        def manual():
            return self.ui(messagebox.askokcancel, "Press the program button",
                           "The NuEVI didn't switch to its bootloader by itself.\n\nOpen the top cover and press the "
                           "small white button on the Teensy once, then click OK (you have 60 seconds).\n\n"
                           "Cancel leaves the NuEVI unchanged.")
        tf.flash(LOADER, hex_path, board, log=self.log, manual_prompt=manual)
        self.backed_up_this_session = False  # the device content has changed
        self.device_info = None
        tip = ""
        old = meta.get("settings_version")
        if isinstance(old, int):
            matches = [i for i in self.backup_tree.get_children() if str(self.backup_tree.item(i)["values"][2]) == str(old)]
            if matches:
                tip = (f"\n\nIf you want your settings exactly as they were with this firmware, restore the backup "
                       f"\"{matches[0]}\" (Settings tab).")
        self.ui(messagebox.showinfo, "Done", "Firmware flashed. The NuEVI keeps its settings." + tip)

    def _start_flash(self, hex_path):
        if not os.path.exists(LOADER):
            messagebox.showerror("Flash", "teensy_loader_cli.exe is missing from " + LOADER)
            return
        if self.device["state"] == "none":
            messagebox.showerror("Flash", "No NuEVI connected. Plug it in via USB first.")
            return
        ok, why = tf.hex_valid(hex_path, self.cfg["board"])
        if not ok:
            messagebox.showerror("Flash", f"Won't flash {os.path.basename(hex_path)}: {why}")
            return
        meta = read_json(hex_path[:-4] + ".json")
        if not messagebox.askyesno("Flash firmware", f"Flash this firmware to your {tf.BOARDS[self.cfg['board']]['label']}?\n\n"
                                   f"{meta.get('label', '')}\n{os.path.basename(hex_path)}\n"
                                   f"firmware {meta.get('firmware', '?')}, settings v{meta.get('settings_version', '?')}"):
            return
        b = self._ensure_backup()
        if b is False:
            return
        self.run_task(self._flash_task, hex_path, b == "backup")

    def flash_selected(self):
        sel = self.fw_tree.selection()
        if not sel:
            messagebox.showinfo("Flash", "Select a firmware first.")
            return
        self._start_flash(os.path.join(FWDIR, sel[0]))

    def rollback(self):
        originals = [n for n in os.listdir(FWDIR) if n.endswith(".hex") and
                     read_json(os.path.join(FWDIR, n[:-4] + ".json")).get("original")]
        if not originals:
            messagebox.showinfo("Roll back", "No firmware is marked as the original. Select one and use Flash selected.")
            return
        self._start_flash(os.path.join(FWDIR, originals[0]))

    def import_hex(self):
        path = filedialog.askopenfilename(title="Import firmware", filetypes=[("Intel HEX", "*.hex")])
        if not path:
            return
        ok, why = tf.hex_valid(path, self.cfg["board"])
        if not ok:
            messagebox.showerror("Import", why)
            return
        dest = os.path.join(FWDIR, os.path.basename(path))
        shutil.copy2(path, dest)
        with open(dest[:-4] + ".json", "w") as fh:
            json.dump({"label": "Imported", "board": self.cfg["board"], "source": path,
                       "built": time.strftime("%Y-%m-%d %H:%M")}, fh, indent=1)
        self.refresh_firmware()
        self.log("Imported " + path)

    # ------------------------------------------------------------------ develop
    def _unc(self, linux_path):
        return "\\\\wsl.localhost\\" + self.cfg["distro"] + linux_path.replace("/", "\\")

    def _wsl(self, cmd, timeout=900, stream=True):
        p = subprocess.Popen(["wsl.exe", "-d", self.cfg["distro"], "--", "bash", "-lc", cmd],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        lines = []
        start = time.time()
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            lines.append(line)
            if stream and line:
                self.log("  " + line)
            if time.time() - start > timeout:
                p.kill()
                raise RuntimeError("Timed out")
        p.wait()
        return p.returncode, lines

    def _task_git_status(self):
        repo = self.cfg["repo"]
        rc, out = self._wsl(f"cd '{repo}' && git rev-parse --abbrev-ref HEAD && git rev-parse --short HEAD && "
                            f"git status --porcelain | wc -l && grep -o 'FIRMWARE_VERSION \"[^\"]*\"' NuEVI/config.h && "
                            f"grep -o 'EEPROM_VERSION [0-9]*' NuEVI/settings.h", stream=False)
        if rc != 0 or len(out) < 5:
            self.q.put(("call", lambda: self.git_var.set(f"Source: could not read {repo} in WSL ({self.cfg['distro']})")))
            return
        branch, commit, dirty, fw, ee = out[:5]
        fw = fw.split('"')[1]
        ee = ee.split()[1]
        txt = (f"Source: {repo}   (WSL {self.cfg['distro']})\nBranch {branch}, commit {commit}"
               f"{', ' + dirty + ' changed files not committed' if dirty != '0' else ''}.   "
               f"Firmware version {fw}, settings version {ee}.")
        self.q.put(("call", lambda: self.git_var.set(txt)))
        return {"commit": commit, "dirty": dirty != "0", "firmware": fw, "settings_version": int(ee), "branch": branch}

    def _task_build(self):
        info = self._task_git_status()
        if not info:
            raise RuntimeError("Could not read the source repository in WSL")
        env = tf.BOARDS[self.cfg["board"]]["env"]
        self.log(f"Building env '{env}' from {self.cfg['repo']} ...")
        rc, out = self._wsl(f"cd '{self.cfg['repo']}/NuEVI' && ~/.platformio/penv/bin/pio run -e {env}")
        if rc != 0:
            raise RuntimeError("Build failed - see the log for compiler errors")
        src = self._unc(f"{self.cfg['repo']}/NuEVI/.pio/build/{env}/firmware.hex")
        tag = info["commit"] + ("-modified" if info["dirty"] else "")
        name = f"NuEVI-{self.cfg['board']}_{info['firmware']}_{tag}_{time.strftime('%Y%m%d-%H%M%S')}.hex"
        dest = os.path.join(FWDIR, name)
        shutil.copy2(src, dest)
        with open(dest[:-4] + ".json", "w") as fh:
            json.dump({"label": "Built from source", "firmware": info["firmware"],
                       "settings_version": info["settings_version"], "board": self.cfg["board"],
                       "source": f"{info['branch']} @ {tag}", "built": time.strftime("%Y-%m-%d %H:%M")}, fh, indent=1)
        self.log("Build OK -> firmware library: " + name)
        self.q.put(("call", self.refresh_firmware))
        return dest

    def build_and_flash(self):
        if self.device["state"] == "none":
            messagebox.showerror("Build & flash", "No NuEVI connected.")
            return
        b = self._ensure_backup()
        if b is False:
            return

        def task():
            dest = self._task_build()
            if not self.ui(messagebox.askyesno, "Build & flash", f"Build succeeded.\n\nFlash {os.path.basename(dest)} now?"):
                return
            self._flash_task(dest, b == "backup")
        self.run_task(task)

    def _task_tests(self):
        self.log("Running chord tests...")
        rc, out = self._wsl(f"cd '{self.cfg['repo']}/tools' && make -s && ./chordtest 2>&1 | tail -15")
        self.log("Chord tests " + ("PASSED" if rc == 0 and any("passed" in l.lower() for l in out) else "FAILED"))

    def _vscode(self, linux_path):
        try:
            subprocess.Popen(f'code --remote wsl+{self.cfg["distro"]} "{linux_path}"', shell=True, creationflags=NO_WINDOW)
        except OSError as e:
            messagebox.showerror("VS Code", f"Could not start VS Code: {e}")


if __name__ == "__main__":
    App().mainloop()
