#!/usr/bin/env python3
"""Retro-Go Assistant - Windows GUI front-end for rg_tool.py.
Never modifies ESP-IDF: everything is done through process environment only."""
import ast, glob, json, os, queue, re, shutil, subprocess, sys, tempfile, threading, time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

CFG = os.path.join(os.path.expanduser("~"), ".retrogo_assistant.json")
OLD_CFG = os.path.join(os.path.expanduser("~"), ".retrogo_manager.json")  # migrated automatically
BASE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
NEWCON = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
CTRL = re.compile("[\x00-\x08\x0b-\x1f\x7f]")
PY = "@IDFPY@"  # placeholder replaced by the ESP-IDF python once detected
HELPER = r'''
import sys, time, threading, serial
port, bsel, rst = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
out = sys.stdout.buffer
S = [None]
def say(m):
    out.write(("\n[rgm] " + m + "\n").encode()); out.flush()
def openp(b, tries=60):
    for _ in range(tries):
        try:
            s = serial.Serial(); s.port = port; s.baudrate = b; s.timeout = 0.1
            s.dtr = False; s.rts = False; s.open(); return s
        except Exception:
            time.sleep(0.25)
    say("cannot open " + port); sys.exit(1)
def reset(s):
    try:
        s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
    except Exception:
        pass
def score(d):
    return 0 if len(d) < 16 else sum(32 <= c < 127 or c in (9, 10, 13) for c in d) / len(d)
baud = int(bsel) if bsel.isdigit() else 115200
s = openp(baud); S[0] = s
if bsel == "auto":
    best = (0, 115200)
    for b in (115200, 1152000, 921600, 460800, 230400):
        s.baudrate = b; s.reset_input_buffer(); reset(s)
        end = time.time() + 1.5; d = b""
        while time.time() < end:
            d += s.read(4096)
        sc = score(d)
        if sc > best[0]: best = (sc, b)
        if sc > 0.95: break
    baud = best[1]; s.baudrate = baud
    say("auto baud: %d%s" % (baud, "" if best[0] else " (no readable data seen at any baud)"))
def rd():
    for line in sys.stdin.buffer:
        line = line.rstrip(b"\r\n")
        try:
            if line == b"\x00RESET": reset(S[0])
            else: S[0].write(line + b"\n")
        except Exception:
            pass
threading.Thread(target=rd, daemon=True).start()
if rst: reset(s)
while True:
    try:
        d = s.read(4096)
        if d: out.write(d); out.flush()
    except Exception:
        say("port lost - reconnecting")
        try: s.close()
        except Exception: pass
        s = openp(baud); S[0] = s; say("reconnected")
'''


def q(s):  # PowerShell single-quote escape
    return "'" + str(s).replace("'", "''") + "'"


def load_cfg():
    try:
        with open(CFG if os.path.exists(CFG) else OLD_CFG, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


# ---------------------------------------------------------------- discovery
def valid_project(p):
    return bool(p) and os.path.isfile(os.path.join(p, "rg_tool.py"))


def scan_apps(p):
    src = open(os.path.join(p, "rg_tool.py"), encoding="utf-8", errors="replace").read()
    apps = []
    try:
        for n in ast.walk(ast.parse(src)):
            if (isinstance(n, ast.Assign) and isinstance(n.value, ast.Dict)
                    and any(isinstance(t, ast.Name) and t.id == "PROJECT_APPS" for t in n.targets)):
                apps = [k.value for k in n.value.keys if isinstance(k, ast.Constant) and isinstance(k.value, str)]
                break
    except SyntaxError:
        pass
    if not apps:
        m = re.search(r"PROJECT_APPS\s*=\s*\{(.*?)^\}", src, re.S | re.M)
        if m:
            apps = re.findall(r"^\s*[\"']([\w.-]+)[\"']\s*:", m.group(1), re.M)
    if not apps:  # last resort: folders that look like apps
        apps = sorted(d for d in os.listdir(p) if d not in ("components", "tools")
                      and os.path.isfile(os.path.join(p, d, "CMakeLists.txt")))
    return apps


def scan_targets(p):
    d = os.path.join(p, "components", "retro-go", "targets")
    if not os.path.isdir(d):
        return []
    return sorted(t for t in os.listdir(d) if os.path.isfile(os.path.join(d, t, "config.h")))


def com_ports():
    ports = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as k:
            i = 0
            while True:
                try:
                    ports.append(winreg.EnumValue(k, i)[1])
                    i += 1
                except OSError:
                    break
    except Exception:
        pass
    return sorted(set(ports), key=lambda s: int(re.sub(r"\D", "", s) or 0))


def find_idf():
    home = os.path.expanduser("~")
    cand = [os.environ.get("IDF_PATH", ""), home + r"\esp\esp-idf", home + r"\Desktop\esp-idf", r"C:\Espressif\esp-idf"]
    for root in (r"C:\Espressif\frameworks", home + r"\esp"):
        cand += sorted(glob.glob(root + r"\esp-idf*"), reverse=True)
    for p in cand:
        if p and os.path.isfile(os.path.join(p, "tools", "idf.py")):
            return p
    return ""


def tools_path(idf):
    t = os.environ.get("IDF_TOOLS_PATH")
    if t:
        return t
    root = os.path.dirname(os.path.dirname(idf))
    for r in (root, r"C:\Espressif"):
        if os.path.isdir(os.path.join(r, "python_env")):
            return r
    return os.path.join(os.path.expanduser("~"), ".espressif")


def find_idf_python(idf, tools):
    e = os.environ.get("IDF_PYTHON_ENV_PATH")
    if e and os.path.isfile(os.path.join(e, "Scripts", "python.exe")):
        return os.path.join(e, "Scripts", "python.exe")
    m = re.search(r"(\d+)\.(\d+)", os.path.basename(idf))
    pats = ([f"idf{m[1]}.{m[2]}_py*_env"] if m else []) + ["idf*_env"]
    for pat in pats:
        for d in sorted(glob.glob(os.path.join(tools, "python_env", pat)), reverse=True):
            py = os.path.join(d, "Scripts", "python.exe")
            if os.path.isfile(py):
                return py
    return ""


def build_env(idf, tools, py):
    """ESP-IDF env MERGED with the normal Windows PATH (git etc. stay available)."""
    env = dict(os.environ)
    env.update(IDF_PATH=idf, IDF_TOOLS_PATH=tools, PYTHONUTF8="1", PYTHONIOENCODING="utf-8",
               IDF_PYTHON_ENV_PATH=os.path.dirname(os.path.dirname(py)))
    env.pop("MSYSTEM", None)
    extra = []
    try:
        out = subprocess.run([py, os.path.join(idf, "tools", "idf_tools.py"), "export", "--format", "key-value"],
                             env=env, capture_output=True, text=True, timeout=90, creationflags=NOWIN).stdout
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                if k.upper() == "PATH":
                    extra = [x for x in v.split(os.pathsep) if x and "%PATH%" not in x and x != "$PATH"]
                else:
                    env[k] = v
    except Exception:
        pass
    env["PATH"] = os.pathsep.join([os.path.dirname(py)] + extra + [os.path.join(idf, "tools"), env.get("PATH", "")])
    return env


def chip_of(sdkconfig):
    try:
        m = re.search(r'CONFIG_IDF_TARGET="(\w+)"', open(sdkconfig, errors="replace").read())
        if m:
            return m[1]
    except Exception:
        pass
    return "esp32s3"


def toolchain_prefix(chip):
    return {"esp32": "xtensa-esp32-elf-", "esp32s2": "xtensa-esp32s2-elf-",
            "esp32s3": "xtensa-esp32s3-elf-"}.get(chip, "riscv32-esp-elf-")


# ---------------------------------------------------------------- GUI
class Manager(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Retro-Go Assistant")
        self.geometry("1500x880")
        self.cfg = load_cfg()
        self.q = queue.Queue()
        self.busy, self.proc, self.stopped = False, None, False
        self.env, self.py, self.env_idf = None, "", None
        self.cur_tab, self.mon_proc, self.mon_app_last, self.tabs = "log", None, None, {}
        self.apps, self.vars, self.cards, self.status = [], {}, {}, {}
        sv = lambda k, d="": tk.StringVar(value=self.cfg.get(k, d))
        self.proj, self.idf, self.target = sv("project"), sv("idf"), sv("target")
        self.port, self.baud, self.mon = sv("port", "COM4"), sv("baud", "1152000"), sv("monitor", "(auto)")
        self.mbaud = sv("mbaud", "auto")
        self.build_ui()
        near = next((d for d in (BASE, os.path.dirname(BASE), os.path.dirname(os.path.dirname(BASE)))
                     if valid_project(d)), None)
        if near:
            self.proj.set(near)
        elif not valid_project(self.proj.get()):
            p = filedialog.askdirectory(title="Select the Retro-Go project (folder containing rg_tool.py)")
            self.proj.set(os.path.normpath(p) if p else "")
        if not os.path.isfile(os.path.join(self.idf.get() or "-", "tools", "idf.py")):
            self.idf.set(find_idf())
        self.reload()
        self.after(50, self.pump)
        self.protocol("WM_DELETE_WINDOW", self.close)

    # ---- layout
    def build_ui(self):
        P = dict(padx=4, pady=3)
        top = ttk.Frame(self)
        top.pack(fill="x", padx=8, pady=6)
        top.columnconfigure(1, weight=1)

        def row(r, label, widget_fn):
            ttk.Label(top, text=label).grid(row=r, column=0, sticky="w", **P)
            widget_fn()

        def r_proj():
            ttk.Entry(top, textvariable=self.proj).grid(row=0, column=1, sticky="ew", **P)
            f = ttk.Frame(top); f.grid(row=0, column=2, sticky="w")
            for t, c in (("Browse...", self.browse_project), ("Reload", self.reload),
                         ("Edit rg_tool.py", lambda: self.open_path("rg_tool.py"))):
                ttk.Button(f, text=t, command=c).pack(side="left", **P)
        row(0, "Project:", r_proj)

        def r_tgt():
            f = ttk.Frame(top); f.grid(row=1, column=1, columnspan=2, sticky="w")
            self.cb_target = ttk.Combobox(f, textvariable=self.target, width=22, state="readonly")
            self.cb_target.pack(side="left", **P)
            ttk.Button(f, text="Open Target Config", command=self.open_target).pack(side="left", **P)
            ttk.Label(f, text="   COM:").pack(side="left")
            self.cb_port = ttk.Combobox(f, textvariable=self.port, width=9)
            self.cb_port.pack(side="left", **P)
            ttk.Button(f, text="Refresh", command=self.refresh_ports).pack(side="left", **P)
            ttk.Label(f, text="   Flash baud:").pack(side="left")
            ttk.Entry(f, textvariable=self.baud, width=10).pack(side="left", **P)
        row(1, "Target:", r_tgt)

        def r_idf():
            ttk.Entry(top, textvariable=self.idf).grid(row=2, column=1, sticky="ew", **P)
            f = ttk.Frame(top); f.grid(row=2, column=2, sticky="w")
            for t, c in (("ESP-IDF...", self.browse_idf), ("Check Environment", self.check_env)):
                ttk.Button(f, text=t, command=c).pack(side="left", **P)
        row(2, "ESP-IDF:", r_idf)

        # apps
        lf = ttk.LabelFrame(self, text="Apps")
        lf.pack(fill="both", expand=False, padx=8, pady=4)
        bar = ttk.Frame(lf); bar.pack(fill="x")
        ttk.Button(bar, text="Select All", command=lambda: self.set_all(True)).pack(side="left", **P)
        ttk.Button(bar, text="Select None", command=lambda: self.set_all(False)).pack(side="left", **P)
        ttk.Label(bar, text="(right-click an app for actions)").pack(side="right", **P)
        self.canvas = tk.Canvas(lf, height=190, highlightthickness=0)
        sb = ttk.Scrollbar(lf, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y"); self.canvas.pack(side="left", fill="both", expand=True)
        self.grid_fr = ttk.Frame(self.canvas)
        self.canvas.create_window((0, 0), window=self.grid_fr, anchor="nw", tags="g")
        self.canvas.bind("<Configure>", lambda e: (self.canvas.itemconfig("g", width=e.width), self.layout_cards(e.width)))
        self.grid_fr.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Enter>", lambda e: self.canvas.bind_all(
            "<MouseWheel>", lambda ev: self.canvas.yview_scroll(int(-ev.delta / 120), "units")))
        self.canvas.bind("<Leave>", lambda e: self.canvas.unbind_all("<MouseWheel>"))

        # actions
        act = ttk.Frame(self); act.pack(fill="x", padx=8, pady=4)
        self.btns = []
        for t, c in (("Build Selected", self.a_build), ("Build + Flash Selected", self.a_build_flash),
                     ("Build Image", self.a_build_img), ("Flash Selected", self.a_flash),
                     ("Flash Full Image", self.a_flash_img), ("Clean Selected", self.a_clean),
                     ("Monitor", self.a_monitor)):
            b = ttk.Button(act, text=t, command=c); b.pack(side="left", **P); self.btns.append(b)
        ttk.Button(act, text="Stop", command=self.stop).pack(side="right", **P)
        self.state_lbl = ttk.Label(act, text="Idle"); self.state_lbl.pack(side="right", padx=10)

        mb = ttk.Menubutton(act, text="Tools \u25be"); mb.pack(side="right", **P)
        tm = tk.Menu(mb, tearoff=0); mb["menu"] = tm
        for t, c in (("Create run_assistant.bat launcher", self.tool_launcher),
                     ("Build standalone .exe (PyInstaller)", self.tool_exe),
                     ("Open assistant folder", lambda: os.startfile(BASE)),
                     ("Open settings file", lambda: os.path.exists(CFG) and os.startfile(CFG))):
            tm.add_command(label=t, command=c)

        pw = ttk.PanedWindow(self, orient="horizontal"); pw.pack(fill="both", expand=True, padx=8, pady=4)
        self.texts = {}
        self.mon_rst, self.send_var = tk.BooleanVar(value=True), tk.StringVar()
        left = ttk.LabelFrame(pw, text="Build / Flash / Output"); pw.add(left, weight=1)
        lb = ttk.Frame(left); lb.pack(fill="x")
        ttk.Button(lb, text="Copy Logs", command=lambda: self.copy_logs("log")).pack(side="left", **P)
        ttk.Button(lb, text="Clear Logs", command=lambda: self.texts["log"].delete("1.0", "end")).pack(side="left", **P)
        self.texts["log"] = self.mk_text(left)
        right = ttk.LabelFrame(pw, text="Monitor"); pw.add(right, weight=1)
        r1 = ttk.Frame(right); r1.pack(fill="x")
        self.btn_mon = ttk.Button(r1, text="Start Monitor", command=self.toggle_monitor); self.btn_mon.pack(side="left", **P)
        ttk.Button(r1, text="Reset Board", command=lambda: self.mon_send("\x00RESET")).pack(side="left", **P)
        ttk.Button(r1, text="Copy Logs", command=lambda: self.copy_logs("monitor")).pack(side="left", **P)
        ttk.Button(r1, text="Clear Logs", command=lambda: self.texts["monitor"].delete("1.0", "end")).pack(side="left", **P)
        self.rx_lbl = ttk.Label(r1, text="RX: 0 bytes"); self.rx_lbl.pack(side="right", **P)
        r2 = ttk.Frame(right); r2.pack(fill="x")
        ttk.Label(r2, text="ELF app:").pack(side="left", **P)
        self.cb_mon = ttk.Combobox(r2, textvariable=self.mon, width=16, state="readonly"); self.cb_mon.pack(side="left", **P)
        ttk.Label(r2, text="Baud:").pack(side="left", **P)
        ttk.Combobox(r2, textvariable=self.mbaud, width=9, values=["auto", "115200", "230400", "460800", "921600", "1152000"]).pack(side="left", **P)
        ttk.Checkbutton(r2, text="Reset on start", variable=self.mon_rst).pack(side="left", **P)
        r3 = ttk.Frame(right); r3.pack(side="bottom", fill="x")
        ttk.Label(r3, text="Send:").pack(side="left", **P)
        e = ttk.Entry(r3, textvariable=self.send_var); e.pack(side="left", fill="x", expand=True, **P)
        send = lambda *_: (self.mon_send(self.send_var.get()), self.send_var.set(""))
        e.bind("<Return>", send)
        ttk.Button(r3, text="Send", command=send).pack(side="left", **P)
        self.texts["monitor"] = self.mk_text(right)

    def mk_text(self, parent):
        t = tk.Text(parent, wrap="char", font=("Consolas", 9), bg="#111", fg="#ddd", insertbackground="#ddd")
        ys = ttk.Scrollbar(parent, command=t.yview); t.configure(yscrollcommand=ys.set)
        ys.pack(side="right", fill="y"); t.pack(fill="both", expand=True)
        return t

    # ---- project / lists
    def reload(self):
        p = self.proj.get()
        for w in self.grid_fr.winfo_children():
            w.destroy()
        self.cards.clear(); self.status.clear()
        if not valid_project(p):
            self.apps = []; self.cb_target["values"] = []
            self.log(f"No valid Retro-Go project selected (rg_tool.py not found): {p!r}\n")
            return
        old = {a: v.get() for a, v in self.vars.items()}
        saved = set(self.cfg.get("apps", []))
        self.apps = scan_apps(p)
        self.vars = {a: tk.BooleanVar(value=old.get(a, a in saved)) for a in self.apps}
        targets = scan_targets(p)
        self.cb_target["values"] = targets
        if self.target.get() not in targets:
            self.target.set(targets[0] if targets else "")
        self.cb_mon["values"] = ["(auto)"] + self.apps
        if self.mon.get() not in self.cb_mon["values"]:
            self.mon.set("(auto)")
        for a in self.apps:
            card = ttk.Frame(self.grid_fr, relief="groove", borderwidth=1, padding=4)
            ttk.Checkbutton(card, text=a, variable=self.vars[a]).pack(anchor="w")
            self.status[a] = ttk.Label(card, text="", foreground="#666"); self.status[a].pack(anchor="w")
            for w in (card, *card.winfo_children()):
                w.bind("<Button-3>", lambda e, a=a: self.ctx_menu(e, a))
            self.cards[a] = card
        self.layout_cards(self.canvas.winfo_width())
        self.refresh_status(); self.refresh_ports()
        self.log(f"Project: {p}\n{len(self.apps)} apps, {len(targets)} target(s): {', '.join(targets)}\n")

    def layout_cards(self, width):
        cols = max(1, width // 190)
        for i, a in enumerate(self.apps):
            if a in self.cards:
                self.cards[a].grid(row=i // cols, column=i % cols, sticky="ew", padx=3, pady=3)
        for c in range(cols):
            self.grid_fr.columnconfigure(c, weight=1)

    def refresh_status(self):
        for a, lbl in self.status.items():
            ok = os.path.isfile(os.path.join(self.proj.get(), a, "build", a + ".elf"))
            lbl.config(text="● built" if ok else "○ not built", foreground="#2a7" if ok else "#888")

    def refresh_ports(self):
        ports = com_ports()
        self.cb_port["values"] = ports
        if ports and self.port.get() not in ports:
            self.port.set(ports[0])

    def set_all(self, v):
        for x in self.vars.values():
            x.set(v)

    def sel(self):
        s = [a for a in self.apps if self.vars[a].get()]
        if not s:
            messagebox.showinfo("Retro-Go Assistant", "Select at least one app first.")
        return s

    def browse_project(self):
        p = filedialog.askdirectory(title="Select Retro-Go project")
        if p:
            if not valid_project(p):
                messagebox.showerror("Not a Retro-Go project", "rg_tool.py was not found in that folder.")
                return
            self.proj.set(os.path.normpath(p)); self.reload()

    def browse_idf(self):
        p = filedialog.askdirectory(title="Select ESP-IDF folder (contains tools/idf.py)")
        if p:
            self.idf.set(os.path.normpath(p)); self.env = None

    # ---- open helpers
    def open_path(self, *parts):
        path = os.path.join(self.proj.get(), *parts)
        if os.path.exists(path):
            os.startfile(path)
        else:
            self.log(f"Not found: {path}\n")

    def open_target(self):
        t = os.path.join("components", "retro-go", "targets", self.target.get())
        self.open_path(t)

    def ctx_menu(self, ev, a):
        m = tk.Menu(self, tearoff=0)
        items = [("Open Folder", lambda: self.open_path(a)),
                 ("Open Build Folder", lambda: self.open_path(a, "build")),
                 ("Open CMakeLists.txt", lambda: self.open_path(a, "CMakeLists.txt")),
                 ("Open sdkconfig", lambda: self.open_path(a, "sdkconfig")),
                 ("Edit rg_tool.py", lambda: self.open_path("rg_tool.py")), None,
                 ("Build", lambda: self.run([("Build " + a, self.rg("build", [a]))])),
                 ("Build + Flash", lambda: self.run([("Build " + a, self.rg("build", [a])),
                                                     ("Flash " + a, self.rg("flash", [a], True))])),
                 ("Flash", lambda: self.run([("Flash " + a, self.rg("flash", [a], True))])),
                 ("Clean", lambda: self.run([("Clean " + a, self.rg("clean", [a]))])),
                 ("Monitor (this app)", lambda: self.a_monitor(a))]
        for it in items:
            if it is None:
                m.add_separator()
            else:
                m.add_command(label=it[0], command=it[1])
        m.tk_popup(ev.x_root, ev.y_root)

    # ---- commands (all go through rg_tool.py)
    def rg(self, cmd, apps, port=False):
        c = [PY, os.path.join(self.proj.get(), "rg_tool.py"), cmd] + list(apps)
        if self.target.get():
            c += ["--target", self.target.get()]
        if port:
            c += ["--port", self.port.get(), "--baud", self.baud.get()]
        return c

    def a_build(self):
        s = self.sel(); s and self.run([("Build", self.rg("build", s))])

    def a_build_flash(self):
        s = self.sel(); s and self.run([("Build", self.rg("build", s)), ("Flash", self.rg("flash", s, True))])

    def a_build_img(self):
        s = self.sel(); s and self.run([("Build Image", self.rg("build-img", s))])

    def a_flash(self):
        s = self.sel(); s and self.run([("Flash", self.rg("flash", s, True))])

    def a_flash_img(self):
        s = self.sel(); s and self.run([("Flash Full Image (install)", self.rg("install", s, True))])

    def a_clean(self):
        s = self.sel(); s and self.run([("Clean", self.rg("clean", s))])

    def a_monitor(self, app=None):
        threading.Thread(target=self._start_monitor, args=(app,), daemon=True).start()

    def toggle_monitor(self):
        self.stop_monitor() if self.mon_proc else self.a_monitor()

    def check_env(self):
        self.run([("Check environment", self._report)])

    # ---- worker
    def tab_of(self, label):
        return "log"

    def log(self, s, tab=None):
        self.q.put(("log", (tab or self.cur_tab, s)))

    def pump(self):
        out = {}
        try:
            for _ in range(3000):
                k, v = self.q.get_nowait()
                if k == "log":
                    out.setdefault(v[0], []).append(v[1])
                elif k == "monbtn":
                    self.btn_mon.config(text=v)
                elif k == "rx":
                    self.rx_lbl.config(text=f"RX: {v:,} bytes")
                elif k == "state":
                    self.state_lbl.config(text=v)
                    for b in self.btns:
                        b.state(["disabled"] if self.busy else ["!disabled"])
                    if not self.busy:
                        self.refresh_status()
        except queue.Empty:
            pass
        for tab, parts in out.items():
            t = self.texts[tab]; t.insert("end", "".join(parts))
            if int(t.index("end-1c").split(".")[0]) > 8000:
                t.delete("1.0", "2500.0")
            t.see("end")
        self.after(50, self.pump)

    def run(self, steps):
        if self.busy:
            self.log("A task is already running (use Stop to cancel it).\n"); return
        self.save()
        self.busy, self.stopped = True, False
        self.q.put(("state", "Working..."))
        threading.Thread(target=self._work, args=(steps,), daemon=True).start()

    def stream(self, argv, env=None, cwd=None):
        self.log(f"\n$ {' '.join(argv)}\n")
        self.proc = subprocess.Popen(argv, cwd=cwd or self.proj.get(), env=env or self.env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=NOWIN)
        for raw in iter(self.proc.stdout.readline, b""):
            self.log(ANSI.sub("", raw.decode("utf-8", "replace")).replace("\r\n", "\n").replace("\r", "\n"))
        return self.proc.wait()

    def _work(self, steps):
        result, resume = "Idle", False
        try:
            self.cur_tab = self.tab_of(steps[0][0])
            if self.mon_proc and any(l.split()[0].lower() == "flash" for l, _ in steps):
                resume = True; self.stop_monitor(wait=True)  # free the COM port for flashing
            if not steps[0][0].startswith("Tools") and not self.prepare():
                result = "Environment error"; return
            for label, job in steps:
                self.cur_tab = self.tab_of(label)
                self.q.put(("state", label + "..."))
                if callable(job):
                    job(); continue
                rc = self.stream([self.py if a == PY else a for a in job])
                if self.stopped:
                    self.log("\n=== Stopped by user ===\n"); result = "Stopped"; return
                if rc != 0:
                    self.log(f"\n=== FAILED: {label} (exit code {rc}) - remaining steps skipped ===\n")
                    result = f"Failed: {label}"; return
                self.log(f"\n=== OK: {label} ===\n")
            result = "Done"
        except Exception as e:
            self.log(f"\nError: {e}\n"); result = "Error"
        finally:
            self.proc, self.busy, self.cur_tab = None, False, "log"
            self.q.put(("state", result))
            if resume:
                threading.Thread(target=self._start_monitor, args=(self.mon_app_last,), daemon=True).start()

    def stop(self):
        if self.proc and self.busy:
            self.stopped = True
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"], creationflags=NOWIN,
                           capture_output=True)

    def prepare(self):
        idf = self.idf.get()
        if self.env and self.env_idf == idf:
            return True
        if not os.path.isfile(os.path.join(idf, "tools", "idf.py")):
            self.log(f"\nESP-IDF not found at {idf!r}. Use the 'ESP-IDF...' button to select it "
                     "(folder containing tools/idf.py).\n"); return False
        tools = tools_path(idf)
        py = find_idf_python(idf, tools)
        if not py:
            self.log(f"\nNo ESP-IDF Python environment found under {tools}\\python_env.\n"
                     "Run the ESP-IDF installer / install.bat once. Refusing to fall back to the system Python.\n")
            return False
        ok = subprocess.run([py, "-c", "import esp_idf_monitor"], capture_output=True, creationflags=NOWIN).returncode == 0
        if not ok:
            self.log(f"\nesp_idf_monitor is not importable from {py}. That Python env looks incomplete.\n")
            return False
        self.log("Preparing ESP-IDF environment...\n")
        self.env, self.py, self.env_idf = build_env(idf, tools, py), py, idf
        self.log(f"ESP-IDF: {idf}\nESP-IDF Python: {py}\nStatus: OK\n")
        return True

    def _report(self):
        def which(n):
            return shutil.which(n, path=self.env["PATH"])
        sysenv = os.environ.get("PATH", "")
        rows = [("Python (system)", shutil.which("python", path=sysenv)), ("Git", which("git")),
                ("CMake", which("cmake")), ("Ninja", which("ninja"))]
        for mod in ("esp_idf_monitor", "esptool"):
            ok = subprocess.run([self.py, "-c", f"import {mod}"], capture_output=True, creationflags=NOWIN).returncode == 0
            rows.append((f"{mod} (ESP-IDF Python)", "importable" if ok else None))
        rows.append(("idf.py", os.path.join(self.idf.get(), "tools", "idf.py")
                     if os.path.isfile(os.path.join(self.idf.get(), "tools", "idf.py")) else None))
        for n, v in rows:
            self.log(f"  {'✓' if v else '✗ MISSING'} {n}: {v or ''}\n")

    # ---- in-GUI serial monitor (pyserial from the ESP-IDF python; no TTY needed)
    def stop_monitor(self, wait=False):
        p = self.mon_proc
        if p:
            subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], creationflags=NOWIN, capture_output=True)
            for _ in range(40 if wait else 0):
                if self.mon_proc is None:
                    break
                time.sleep(0.1)

    def mon_send(self, text):
        p = self.mon_proc
        if not p:
            self.log("Monitor is not running.\n", "monitor"); return
        try:
            p.stdin.write(text.encode("utf-8", "replace") + b"\n"); p.stdin.flush()
        except OSError:
            pass

    def decode_line(self, line, elf, prefix):
        if "Backtrace:" in line:
            addrs = re.findall(r"(0x4[0-9a-fA-F]{7}):0x[0-9a-fA-F]{8}", line)
        else:
            m = re.match(r"\s*(?:PC|MEPC|RA)\s*:\s*(0x4[0-9a-fA-F]{7})", line)
            addrs = [m[1]] if m else []
        tool = shutil.which(prefix + "addr2line", path=self.env["PATH"])
        if not addrs or not tool:
            return ""
        try:
            out = subprocess.run([tool, "-pfiaC", "-e", elf] + addrs, capture_output=True, text=True,
                                 timeout=15, creationflags=NOWIN).stdout
        except Exception:
            return ""
        return "".join("    -> " + l + "\n" for l in out.splitlines())

    def _start_monitor(self, app=None):
        if self.mon_proc:
            self.stop_monitor(wait=True)
        self.save()
        L = lambda s: self.log(s, "monitor")
        if not self.prepare():
            return
        app = app or self.mon.get()
        if not app or app == "(auto)":
            sel = [a for a in self.apps if self.vars[a].get()]
            app = (sel or self.apps or [""])[0]
        self.mon_app_last = app
        p = self.proj.get()
        elf = os.path.join(p, app, "build", app + ".elf")
        elf = elf if os.path.isfile(elf) else None
        prefix = toolchain_prefix(chip_of(os.path.join(p, app, "sdkconfig")))
        hp = os.path.join(tempfile.gettempdir(), "rgm_serial.py")
        with open(hp, "w") as f:
            f.write(HELPER)
        L(f"\n--- Monitor {self.port.get()} @ {self.mbaud.get()} | ELF: {app if elf else 'none (not built)'} ---\n")
        try:
            self.mon_proc = proc = subprocess.Popen(
                [self.py, "-u", hp, self.port.get(), self.mbaud.get(), "1" if self.mon_rst.get() else "0"],
                env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                creationflags=NOWIN)
        except Exception as e:
            L(f"Could not start monitor: {e}\n"); return
        self.q.put(("monbtn", "Stop Monitor"))
        buf, total = "", 0
        while True:
            d = proc.stdout.read1(4096)
            if not d:
                break
            total += len(d); self.q.put(("rx", total))
            t = CTRL.sub("", ANSI.sub("", d.decode("utf-8", "replace")).replace("\r\n", "\n").replace("\r", ""))
            t = t.replace("\ufffd", "?")
            L(t); buf += t
            if len(buf) > 300 and "\n" not in buf:
                L("\n"); buf = ""
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if elf:
                    dec = self.decode_line(line, elf, prefix)
                    if dec:
                        L(dec)
        proc.wait()
        self.mon_proc = None
        self.q.put(("monbtn", "Start Monitor")); L("\n--- Monitor stopped ---\n")

    # ---- tools (what the old .bat files did)
    def tool_launcher(self):
        if getattr(sys, "frozen", False):
            self.log("Already an .exe - no launcher needed.\n"); return
        me = os.path.abspath(__file__)
        bat = os.path.join(os.path.dirname(me), "run_assistant.bat")
        with open(bat, "w") as f:
            f.write('@echo off\r\ncd /d "%~dp0"\r\nstart "" pythonw "' + os.path.basename(me) + '"\r\n')
        self.log(f"Created {bat} (double-click to start the Assistant without a console window).\n")

    def tool_exe(self):
        if getattr(sys, "frozen", False):
            self.log("Already an .exe.\n"); return
        self.run([("Tools: build exe", self._exe)])

    def _exe(self):
        py = shutil.which("python") or shutil.which("py")
        tmp = tempfile.gettempdir()
        for argv in ([py, "-m", "pip", "install", "--user", "pyinstaller"],
                     [py, "-m", "PyInstaller", "--onefile", "--windowed", "--name", "RetroGoAssistant",
                      "--distpath", os.path.join(BASE, "dist"), "--workpath", os.path.join(tmp, "rgm_build"),
                      "--specpath", tmp, os.path.abspath(__file__)]):
            if self.stream(argv, env=dict(os.environ), cwd=BASE) != 0:
                raise RuntimeError("PyInstaller step failed")
        self.log(f"\nDone: {os.path.join(BASE, 'dist', 'RetroGoAssistant.exe')}\n")

    # ---- misc
    def copy_logs(self, key="log"):
        self.clipboard_clear(); self.clipboard_append(self.texts[key].get("1.0", "end"))

    def save(self):
        d = dict(project=self.proj.get(), idf=self.idf.get(), target=self.target.get(), port=self.port.get(),
                 baud=self.baud.get(), mbaud=self.mbaud.get(), monitor=self.mon.get(),
                 apps=[a for a, v in self.vars.items() if v.get()])
        try:
            with open(CFG, "w", encoding="utf-8") as f:
                json.dump(d, f, indent=2)
            self.cfg = d
        except OSError:
            pass

    def close(self):
        self.save(); self.stop_monitor(); self.destroy()


if __name__ == "__main__":
    Manager().mainloop()
