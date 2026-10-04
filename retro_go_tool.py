import ast, os, re, subprocess, sys, threading, tkinter as tk
from pathlib import Path
from tkinter import ttk, filedialog, messagebox

class RetroGoTool(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Retro-Go Tool — Sample")
        self.geometry("1100x720")
        self.minsize(900, 600)
        self.project = tk.StringVar()
        self.target = tk.StringVar()
        self.port = tk.StringVar()
        self.baud = tk.StringVar(value="1152000")
        self.status = tk.StringVar(value="Select a Retro-Go project")
        self.apps, self.vars = [], {}
        self.build_ui()
        self.refresh_ports()

    def build_ui(self):
        root = ttk.Frame(self, padding=10); root.pack(fill="both", expand=True)

        p = ttk.LabelFrame(root, text="Project", padding=8); p.pack(fill="x")
        ttk.Entry(p, textvariable=self.project).pack(side="left", fill="x", expand=True)
        ttk.Button(p, text="Browse…", command=self.choose_project).pack(side="left", padx=5)
        ttk.Button(p, text="Reload", command=self.scan).pack(side="left")

        d = ttk.Frame(root, padding=(0,8)); d.pack(fill="x")
        ttk.Label(d, text="Target:").pack(side="left")
        self.targets = ttk.Combobox(d, textvariable=self.target, state="readonly", width=22)
        self.targets.pack(side="left", padx=5)
        self.targets.bind("<<ComboboxSelected>>", lambda e: self.log("Target: "+self.target.get()+"\n"))
        ttk.Button(d, text="⚙ Open Target Config", command=self.open_target).pack(side="left")
        ttk.Label(d, text="COM:").pack(side="right")
        self.ports = ttk.Combobox(d, textvariable=self.port, state="readonly", width=12)
        self.ports.pack(side="right", padx=5)
        ttk.Button(d, text="↻", width=3, command=self.refresh_ports).pack(side="right")

        box = ttk.LabelFrame(root, text="Projects / Apps", padding=8)
        box.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(box, highlightthickness=0)
        sb = ttk.Scrollbar(box, orient="vertical", command=self.canvas.yview)
        self.area = ttk.Frame(self.canvas)
        self.canvas.create_window((0,0), window=self.area, anchor="nw")
        self.area.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.configure(yscrollcommand=sb.set)
        self.canvas.pack(side="left", fill="both", expand=True); sb.pack(side="right", fill="y")

        a = ttk.Frame(root, padding=(0,8)); a.pack(fill="x")
        for name, cmd in [("Build Selected","build"),("Build Image","build-img"),
                          ("Flash Selected","flash"),("Flash Full Image","install"),
                          ("Clean Selected","clean"),("Monitor","monitor")]:
            ttk.Button(a, text=name, command=lambda c=cmd:self.run_tool(c)).pack(side="left", padx=(0,5))

        out = ttk.LabelFrame(root, text="Output", padding=5); out.pack(fill="both", expand=True)
        self.output = tk.Text(out, height=10); self.output.pack(fill="both", expand=True)
        ttk.Label(root, textvariable=self.status, anchor="w").pack(fill="x", pady=(5,0))

    def choose_project(self):
        p = filedialog.askdirectory(title="Select Retro-Go project folder")
        if p: self.project.set(p); self.scan()

    def scan(self):
        root = self.project.get()
        if not os.path.isdir(root): return
        rg = Path(root)/"rg_tool.py"
        apps = []
        if rg.exists():
            try:
                tree = ast.parse(rg.read_text(encoding="utf-8", errors="replace"))
                for n in tree.body:
                    if isinstance(n, ast.Assign):
                        for t in n.targets:
                            if isinstance(t, ast.Name) and t.id == "PROJECT_APPS":
                                apps = list(ast.literal_eval(n.value).keys())
            except Exception as e: self.log("rg_tool.py read error: "+str(e)+"\n")
        if not apps:
            apps = [p.name for p in Path(root).iterdir() if p.is_dir() and (p/"CMakeLists.txt").exists()]
        self.apps = apps
        for w in self.area.winfo_children(): w.destroy()
        self.vars.clear()
        for i, app in enumerate(apps):
            v = tk.BooleanVar(value=True); self.vars[app] = v
            f = ttk.Frame(self.area, relief="ridge", borderwidth=1, padding=8)
            f.grid(row=i//4, column=i%4, padx=5, pady=5, sticky="ew")
            ttk.Checkbutton(f, text=app, variable=v).pack(anchor="w")
            ttk.Label(f, text="Right-click for actions", foreground="gray").pack(anchor="w")
            for w in (f, *f.winfo_children()):
                w.bind("<Button-3>", lambda e, a=app:self.app_menu(e,a))

        tr = Path(root)/"components"/"retro-go"/"targets"
        targets = sorted([p.name for p in tr.iterdir() if p.is_dir() and (p/"config.h").exists()]) if tr.is_dir() else []
        self.targets["values"] = targets
        if targets: self.target.set("my-handheld" if "my-handheld" in targets else targets[0])
        self.status.set(f"Found {len(apps)} apps, {len(targets)} targets")
        self.log(f"\nFound {len(apps)} apps: {', '.join(apps)}\n")
        self.log(f"Found targets: {', '.join(targets) or '(none)'}\n")

    def app_menu(self, event, app):
        m=tk.Menu(self, tearoff=False)
        m.add_command(label="Open Folder", command=lambda:self.open_path(Path(self.project.get())/app))
        m.add_command(label="Open Build Folder", command=lambda:self.open_path(Path(self.project.get())/app/"build"))
        m.add_separator()
        m.add_command(label="Open CMakeLists.txt", command=lambda:self.open_path(Path(self.project.get())/app/"CMakeLists.txt"))
        m.add_command(label="Open sdkconfig", command=lambda:self.open_path(Path(self.project.get())/app/"sdkconfig"))
        m.add_separator()
        m.add_command(label="Build", command=lambda:self.run_tool("build",[app]))
        m.add_command(label="Clean", command=lambda:self.run_tool("clean",[app]))
        m.tk_popup(event.x_root,event.y_root)

    def open_target(self):
        self.open_path(Path(self.project.get())/"components"/"retro-go"/"targets"/self.target.get())

    def open_path(self, p):
        if not p.exists(): messagebox.showwarning("Not found", str(p)); return
        os.startfile(str(p))

    def refresh_ports(self):
        ports=[]
        if os.name=="nt":
            try:
                import winreg
                k=winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r"HARDWARE\DEVICEMAP\SERIALCOMM")
                i=0
                while True:
                    try: ports.append(winreg.EnumValue(k,i)[1]); i+=1
                    except OSError: break
            except Exception: pass
        self.ports["values"]=sorted(set(ports), key=lambda x:int(re.search(r"\d+",x).group())) if ports else []
        if ports and not self.port.get(): self.port.set(ports[0])

    def run_tool(self, command, explicit=None):
        root=self.project.get(); rg=Path(root)/"rg_tool.py"
        if not rg.exists(): messagebox.showwarning("Project required","Select a folder containing rg_tool.py."); return
        apps=explicit if explicit is not None else [a for a in self.apps if self.vars[a].get()]
        if command!="monitor" and not apps: messagebox.showwarning("No apps","Select at least one app."); return
        cmd=[sys.executable,str(rg),command]+apps
        if self.target.get(): cmd += ["--target",self.target.get()]
        if self.port.get(): cmd += ["--port",self.port.get()]
        cmd += ["--baud",self.baud.get()]
        self.log("\n> "+" ".join(cmd)+"\n"); self.status.set("Running "+command+"…")
        threading.Thread(target=self.proc,args=(cmd,root),daemon=True).start()

    def proc(self,cmd,cwd):
        try:
            p=subprocess.Popen(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
            for line in p.stdout: self.after(0,self.log,line)
            rc=p.wait(); self.after(0,self.status.set,f"Finished: {cmd[2]} (exit {rc})")
        except Exception as e: self.after(0,self.log,"ERROR: "+str(e)+"\n")

    def log(self,s):
        self.output.insert("end",s); self.output.see("end")

if __name__=="__main__":
    RetroGoTool().mainloop()
