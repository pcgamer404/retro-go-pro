#!/usr/bin/env python3
"""
Retro-Go Game Genie Cheat Maker
- Load an existing Retro-Go .cht file
- Fetch Game Genie codes from a GameGenie.com HTML page
- Add/edit/delete cheats
- Enable/disable individual cheats
- Save Retro-Go-compatible .cht files

No third-party Python packages are required.
"""

import re
import urllib.request
import urllib.error
import html
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlparse
import tkinter as tk
from tkinter import ttk, filedialog, messagebox


GG_CHARS = "APZLGITYEOXUKSVN"
GG_RE = re.compile(rf"(?<![A-Z])([{GG_CHARS}]{{6,8}})(?![A-Z])", re.I)


class GameGeniePageParser(HTMLParser):
    """Collect page text while preserving line boundaries from <pre> blocks."""
    def __init__(self):
        super().__init__()
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)


def parse_gamegenie_html(source):
    """
    Parse the classic GameGenie.com NES pages.

    The important fix is preserving the original line structure:
        CODE        Description
        CODE        Description
        CODE + CODE Description

    Previously the parser flattened the whole page into one long string.
    That caused one code to become part of the previous cheat description,
    especially for codes containing N, and caused '+' to become its own row.
    """
    parser = GameGeniePageParser()
    parser.feed(source)

    text = html.unescape("".join(parser.parts))
    code = rf"[{GG_CHARS}]{{6,8}}"
    combo = rf"{code}(?:\s*\+\s*{code}){{0,2}}"

    cheats = []
    seen = set()

    for raw_line in text.splitlines():
        line = " ".join(raw_line.split()).strip()
        if not line:
            continue

        # Ignore navigation/footer/page boilerplate.
        if line in {
            "Site Search",
            "Printer Friendly Version",
        }:
            continue
        if line.startswith("Home: Game Cheats"):
            continue
        if "©" in line or "Danworld, Inc." in line:
            continue

        m = re.match(
            rf"^(?P<codes>{combo})\s+(?P<name>.+?)\s*$",
            line,
            flags=re.I
        )
        if not m:
            continue

        codes = re.sub(r"\s*\+\s*", "+", m.group("codes").upper())
        name = re.sub(r"\s+", " ", m.group("name")).strip()

        # Require a real NES Game Genie code and avoid accidental matches.
        parts = codes.split("+")
        if not all(re.fullmatch(rf"[{GG_CHARS}]{{6,8}}", p, re.I) for p in parts):
            continue

        # Retro-Go does not support Game Genie combinations written as
        # CODE1+CODE2 in a single cheat entry. Split combination cheats into
        # separate entries while preserving the original cheat meaning.
        if len(parts) > 1:
            for part_index, part in enumerate(parts, 1):
                split_name = f"{name} ({part_index}of {len(parts)})"
                unique_key = f"{part}|{split_name}"
                if unique_key in seen:
                    continue
                seen.add(unique_key)
                cheats.append((split_name, part, "OFF"))
        else:
            if codes in seen:
                continue
            seen.add(codes)
            cheats.append((name, codes, "OFF"))

    return cheats


def fetch_gamegenie(url):
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Retro-Go-GameGenie-Cheat-Maker/1.0"}
    )
    with urllib.request.urlopen(req, timeout=15) as response:
        data = response.read()
        charset = response.headers.get_content_charset() or "utf-8"
        return data.decode(charset, errors="replace")


def extract_game_name(source, url):
    """Extract the actual game title from a GameGenie.com page."""
    m = re.search(r"<title[^>]*>(.*?)</title>", source, re.I | re.S)

    if m:
        title = html.unescape(re.sub(r"<[^>]+>", " ", m.group(1)))
        title = re.sub(r"\s+", " ", title).strip()

        # Typical GameGenie title:
        # "GameGenie.com Nintendo (Game Genie) Codes - Adventure Island 2"
        #
        # We want only:
        # "Adventure Island 2"
        patterns = [
            r"^GameGenie\.com\s+Nintendo\s+\(Game Genie\)\s+Codes\s*-\s*(.+)$",
            r"^GameGenie\.com\s+Nintendo\s+Codes\s*-\s*(.+)$",
            r"^Nintendo\s+\(Game Genie\)\s+Codes\s*-\s*(.+)$",
            r"^.*?Codes\s*-\s*(.+)$",
        ]

        for pattern in patterns:
            match = re.match(pattern, title, re.I)
            if match:
                title = match.group(1).strip()
                break

        # Remove common trailing site branding if still present.
        title = re.sub(
            r"\s*[-|:]\s*GameGenie\.com\s*$", "", title, flags=re.I
        ).strip()

        if title and title.lower() not in ("game genie", "gamegenie"):
            return title

    # Fallback: derive the title from the URL slug.
    parsed = urlparse(url)
    slug = Path(parsed.path).stem
    slug = re.sub(r"_?game_genie$", "", slug, flags=re.I)
    name = re.sub(r"[_-]+", " ", slug).strip()

    return name.title() if name else "Game"


def parse_cht(path):
    cheats = []
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        for line_no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split("|")
            if len(parts) < 2:
                continue

            name = parts[0].strip()
            code = parts[1].strip().upper()
            state = parts[2].strip().upper() if len(parts) >= 3 else "OFF"

            if state not in ("ON", "OFF"):
                state = "OFF"

            if name and code:
                cheats.append((name, code, state))

    return cheats


def save_cht(path, cheats):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for name, code, state in cheats:
            # Keep the exact Retro-Go three-field format.
            f.write(f"{name}|{code}|{state}\n")


class CheatMaker(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Retro-Go Game Genie Cheat Maker")
        self.geometry("980x650")
        self.minsize(820, 520)

        self.cheats = []
        self.current_path = None
        self.dirty = False
        self.game_name = "Game"
        self.save_folder = Path.home() / "Retro-Go Cheats"
        self.save_folder.mkdir(parents=True, exist_ok=True)

        self._build_style()
        self._build_ui()

        self.url_var.set(
            "https://gamegenie.com/cheats/gamegenie/nes/ninja_gaiden.html"
        )
        self._refresh_count()

    def _build_style(self):
        style = ttk.Style(self)
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass

        style.configure("Title.TLabel", font=("Segoe UI", 16, "bold"))
        style.configure("Small.TLabel", font=("Segoe UI", 9))
        style.configure("Treeview", rowheight=28, font=("Segoe UI", 10))
        style.configure("Treeview.Heading", font=("Segoe UI", 10, "bold"))

    def _build_ui(self):
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)

        ttk.Label(
            root, text="Retro-Go Game Genie Cheat Maker", style="Title.TLabel"
        ).pack(anchor="w")

        ttk.Label(
            root,
            text="Fetch Game Genie codes, split multi-code cheats for Retro-Go, edit them, and export a .cht file.",
            style="Small.TLabel"
        ).pack(anchor="w", pady=(2, 10))

        # Source row
        source = ttk.LabelFrame(root, text="Game Genie source", padding=8)
        source.pack(fill="x", pady=(0, 8))

        self.url_var = tk.StringVar()
        url_entry = ttk.Entry(source, textvariable=self.url_var)
        url_entry.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ttk.Button(source, text="Fetch Codes", command=self.fetch_codes).pack(side="left")

        # Output folder / detected game
        output = ttk.LabelFrame(root, text="Retro-Go .cht save location", padding=8)
        output.pack(fill="x", pady=(0, 8))

        self.folder_var = tk.StringVar(value=str(self.save_folder))
        ttk.Entry(output, textvariable=self.folder_var).pack(
            side="left", fill="x", expand=True, padx=(0, 6)
        )
        ttk.Button(output, text="Browse...", command=self.choose_save_folder).pack(side="left")

        self.game_var = tk.StringVar(value="Game: —")
        ttk.Label(output, textvariable=self.game_var, width=34).pack(side="left", padx=(12, 0))

        # Main table
        table_frame = ttk.Frame(root)
        table_frame.pack(fill="both", expand=True)

        columns = ("state", "name", "code")
        self.tree = ttk.Treeview(
            table_frame, columns=columns, show="headings", selectmode="extended"
        )
        self.tree.heading("state", text="State")
        self.tree.heading("name", text="Cheat name")
        self.tree.heading("code", text="Game Genie code")
        self.tree.column("state", width=85, anchor="center", stretch=False)
        self.tree.column("name", width=600, anchor="w")
        self.tree.column("code", width=220, anchor="center", stretch=False)

        yscroll = ttk.Scrollbar(table_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=yscroll.set)

        self.tree.pack(side="left", fill="both", expand=True)
        yscroll.pack(side="right", fill="y")

        self.tree.bind("<Double-1>", self.edit_selected)
        self.tree.bind("<Return>", self.toggle_selected)

        # Buttons
        buttons = ttk.Frame(root)
        buttons.pack(fill="x", pady=(8, 5))

        ttk.Button(buttons, text="Load .cht", command=self.load_cht).pack(side="left")
        ttk.Button(buttons, text="Save .cht", command=self.save_cht_as).pack(side="left", padx=5)
        ttk.Button(buttons, text="Save", command=self.save_current).pack(side="left")

        ttk.Separator(buttons, orient="vertical").pack(side="left", fill="y", padx=10)

        ttk.Button(buttons, text="Add Cheat", command=self.add_cheat).pack(side="left")
        ttk.Button(buttons, text="Edit Selected", command=self.edit_selected).pack(side="left", padx=5)
        ttk.Button(buttons, text="Delete Selected", command=self.delete_selected).pack(side="left")

        ttk.Separator(buttons, orient="vertical").pack(side="left", fill="y", padx=10)

        ttk.Button(buttons, text="All OFF", command=self.all_off).pack(side="left")
        ttk.Button(buttons, text="All ON", command=self.all_on).pack(side="left", padx=5)
        ttk.Button(buttons, text="Toggle Selected", command=self.toggle_selected).pack(side="left")

        # Bottom status
        bottom = ttk.Frame(root)
        bottom.pack(fill="x", pady=(5, 0))

        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(bottom, textvariable=self.status_var).pack(side="left")

        self.count_var = tk.StringVar()
        ttk.Label(bottom, textvariable=self.count_var).pack(side="right")

        self.tree.bind("<<TreeviewSelect>>", lambda e: self._refresh_count())
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def _refresh_count(self):
        total = len(self.cheats)
        on = sum(1 for c in self.cheats if c[2] == "ON")
        self.count_var.set(f"{total} cheats  |  {on} ON  |  {total - on} OFF")

    def _mark_dirty(self):
        self.dirty = True
        self._refresh_count()

    def _redraw(self, select_indices=None):
        for item in self.tree.get_children():
            self.tree.delete(item)

        for i, (name, code, state) in enumerate(self.cheats):
            self.tree.insert(
                "", "end", iid=str(i),
                values=(state, name, code)
            )

        if select_indices:
            valid = [str(i) for i in select_indices if 0 <= i < len(self.cheats)]
            self.tree.selection_set(valid)

        self._refresh_count()

    def choose_save_folder(self):
        folder = filedialog.askdirectory(
            title="Choose Retro-Go .cht save folder",
            initialdir=str(self.save_folder)
        )
        if folder:
            self.save_folder = Path(folder)
            self.folder_var.set(str(self.save_folder))
            self.status_var.set(f"Save folder: {self.save_folder}")

    def automatic_cht_path(self):
        folder_text = self.folder_var.get().strip()
        if folder_text:
            self.save_folder = Path(folder_text)

        self.save_folder.mkdir(parents=True, exist_ok=True)

        # Retro-Go NES cheat filename convention:
        # <Game Name>.nes.cht
        filename = re.sub(r'[<>:"/\\|?*]', "", self.game_name).strip()
        filename = re.sub(r"\s+", " ", filename)

        if not filename:
            filename = "Game"

        # Avoid duplicating extensions if the detected title already contains one.
        filename = re.sub(r"\.nes\.cht$", "", filename, flags=re.I)
        filename = re.sub(r"\.cht$", "", filename, flags=re.I)
        filename = re.sub(r"\.nes$", "", filename, flags=re.I)

        filename += ".nes.cht"
        return self.save_folder / filename

    def fetch_codes(self):
        url = self.url_var.get().strip()
        if not url:
            messagebox.showwarning("No URL", "Enter a Game Genie page URL first.")
            return

        try:
            self.status_var.set("Fetching Game Genie page...")
            self.update_idletasks()
            source = fetch_gamegenie(url)
            self.game_name = extract_game_name(source, url)
            self.game_var.set(f"Game: {self.game_name}")
            found = parse_gamegenie_html(source)

            if not found:
                raise ValueError(
                    "No Game Genie codes were found on that page. "
                    "The site layout may have changed."
                )

            # User requested cheats to start disabled.
            self.cheats = [(n, c, "OFF") for n, c, _ in found]
            self.current_path = None
            self.dirty = True
            self._redraw()
            self.status_var.set(f"Imported {len(found)} Game Genie codes.")
        except (urllib.error.URLError, TimeoutError) as e:
            self.status_var.set("Fetch failed.")
            messagebox.showerror("Fetch failed", str(e))
        except Exception as e:
            self.status_var.set("Import failed.")
            messagebox.showerror("Import failed", str(e))

    def load_cht(self):
        path = filedialog.askopenfilename(
            title="Load Retro-Go cheat file",
            filetypes=[("Retro-Go cheat files", "*.cht"), ("All files", "*.*")]
        )
        if not path:
            return

        try:
            cheats = parse_cht(path)
            self.cheats = cheats
            self.current_path = Path(path)
            self.save_folder = self.current_path.parent
            self.folder_var.set(str(self.save_folder))
            self.game_name = self.current_path.stem
            self.game_var.set(f"Game: {self.game_name}")
            self.dirty = False
            self._redraw()
            self.status_var.set(f"Loaded {len(cheats)} cheats from {Path(path).name}")
        except Exception as e:
            messagebox.showerror("Load failed", str(e))

    def save_cht_as(self):
        try:
            path = self.automatic_cht_path()
            save_cht(path, self.cheats)
            self.current_path = path
            self.dirty = False
            self.status_var.set(f"Saved: {path}")
            return True
        except Exception as e:
            messagebox.showerror("Save failed", str(e))
            return False


    def save_current(self):
        if self.current_path is None:
            return self.save_cht_as()

        try:
            save_cht(self.current_path, self.cheats)
            self.dirty = False
            self.status_var.set(f"Saved {self.current_path.name}")
            return True
        except Exception as e:
            messagebox.showerror("Save failed", str(e))
            return False

    def selected_indices(self):
        return [int(i) for i in self.tree.selection()]

    def toggle_selected(self, event=None):
        indices = self.selected_indices()
        if not indices:
            return

        for i in indices:
            name, code, state = self.cheats[i]
            self.cheats[i] = (name, code, "OFF" if state == "ON" else "ON")

        self._mark_dirty()
        self._redraw(indices)

    def all_off(self):
        if not self.cheats:
            return
        self.cheats = [(n, c, "OFF") for n, c, _ in self.cheats]
        self._mark_dirty()
        self._redraw()
        self.status_var.set("All cheats disabled.")

    def all_on(self):
        if not self.cheats:
            return
        self.cheats = [(n, c, "ON") for n, c, _ in self.cheats]
        self._mark_dirty()
        self._redraw()
        self.status_var.set("All cheats enabled.")

    def delete_selected(self):
        indices = self.selected_indices()
        if not indices:
            return

        if not messagebox.askyesno(
            "Delete cheats",
            f"Delete {len(indices)} selected cheat(s)?"
        ):
            return

        for i in sorted(indices, reverse=True):
            del self.cheats[i]

        self._mark_dirty()
        self._redraw()
        self.status_var.set(f"Deleted {len(indices)} cheat(s).")

    def add_cheat(self):
        result = self.cheat_dialog("Add Cheat")
        if result:
            self.cheats.append(result)
            self._mark_dirty()
            self._redraw([len(self.cheats) - 1])
            self.status_var.set("Cheat added.")

    def edit_selected(self, event=None):
        indices = self.selected_indices()
        if len(indices) != 1:
            messagebox.showinfo("Edit Cheat", "Select exactly one cheat to edit.")
            return

        i = indices[0]
        result = self.cheat_dialog("Edit Cheat", self.cheats[i])
        if result:
            self.cheats[i] = result
            self._mark_dirty()
            self._redraw([i])
            self.status_var.set("Cheat updated.")

    def cheat_dialog(self, title, existing=None):
        dialog = tk.Toplevel(self)
        dialog.title(title)
        dialog.transient(self)
        dialog.grab_set()
        dialog.resizable(False, False)

        frame = ttk.Frame(dialog, padding=14)
        frame.pack(fill="both", expand=True)

        name_var = tk.StringVar(value=existing[0] if existing else "")
        code_var = tk.StringVar(value=existing[1] if existing else "")
        state_var = tk.StringVar(value=existing[2] if existing else "OFF")

        ttk.Label(frame, text="Cheat name:").grid(row=0, column=0, sticky="w", pady=4)
        name_entry = ttk.Entry(frame, textvariable=name_var, width=55)
        name_entry.grid(row=0, column=1, sticky="ew", pady=4)

        ttk.Label(frame, text="Game Genie code:").grid(row=1, column=0, sticky="w", pady=4)
        code_entry = ttk.Entry(frame, textvariable=code_var, width=25)
        code_entry.grid(row=1, column=1, sticky="w", pady=4)

        ttk.Label(frame, text="State:").grid(row=2, column=0, sticky="w", pady=4)
        state_box = ttk.Combobox(
            frame, textvariable=state_var,
            values=("OFF", "ON"), state="readonly", width=10
        )
        state_box.grid(row=2, column=1, sticky="w", pady=4)

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, columnspan=2, sticky="e", pady=(12, 0))

        result = {"value": None}

        def accept():
            name = name_var.get().strip()
            code = code_var.get().strip().upper()
            state = state_var.get().upper()

            if not name:
                messagebox.showwarning("Missing name", "Enter a cheat name.", parent=dialog)
                return
            code = re.sub(r"\s*\+\s*", "+", code)
            if not re.fullmatch(rf"[{GG_CHARS}]{{6,8}}", code, re.I):
                messagebox.showwarning(
                    "Invalid code",
                    "Enter one 6- or 8-character NES Game Genie code.",
                    parent=dialog
                )
                return

            result["value"] = (name, code, state)
            dialog.destroy()

        ttk.Button(buttons, text="Cancel", command=dialog.destroy).pack(side="right")
        ttk.Button(buttons, text="OK", command=accept).pack(side="right", padx=(0, 6))

        name_entry.focus_set()
        dialog.bind("<Return>", lambda e: accept())
        dialog.bind("<Escape>", lambda e: dialog.destroy())

        self.wait_window(dialog)
        return result["value"]

    def on_close(self):
        if self.dirty:
            answer = messagebox.askyesnocancel(
                "Unsaved changes",
                "You have unsaved changes. Save before closing?"
            )
            if answer is None:
                return
            if answer and not self.save_current():
                return
        self.destroy()


if __name__ == "__main__":
    CheatMaker().mainloop()
