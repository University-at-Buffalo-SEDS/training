#!/usr/bin/env python3
"""Desktop USB status, session export, and saved print viewer."""
import json
from pathlib import Path
import queue
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from datalogger_client import DeviceError, U32_MAX, export_session, open_client


class App:
    def __init__(self, root):
        self.root = root
        root.title("UBSEDS CrashNBurn Datalogger")
        root.geometry("1000x720")
        self.actions, self.events = queue.Queue(), queue.Queue()
        self.shutdown = threading.Event()
        self.port = tk.StringVar()
        self.state = tk.StringVar(value="Disconnected")
        self.device_state = tk.StringVar(value="Device status unavailable")
        self.output = tk.StringVar(value=str(Path("captures").resolve()))
        self.live_enabled = tk.BooleanVar(value=False)
        self.live_text = tk.StringVar(value="Live sensors disabled")
        top = ttk.Frame(root, padding=10)
        top.pack(fill="x")
        self.ports = ttk.Combobox(top, textvariable=self.port, width=36)
        self.ports.pack(side="left")
        ttk.Button(top, text="Refresh ports", command=self.refresh).pack(side="left")
        ttk.Button(top, text="Connect", command=self.connect).pack(side="left")
        ttk.Button(top, text="Disconnect", command=lambda: self.actions.put(("disconnect", None))).pack(side="left")
        ttk.Label(root, textvariable=self.state, padding=10).pack(anchor="w")
        ttk.Label(root, textvariable=self.device_state, padding=(10, 0), wraplength=960, justify="left").pack(anchor="w")
        controls = ttk.Frame(root, padding=10)
        controls.pack(fill="x")
        for label, action in (("Status", "STATUS"), ("Start", "START"), ("Stop", "STOP"), ("List sessions", "LIST")):
            ttk.Button(controls, text=label, command=lambda a=action: self.actions.put((a, None))).pack(side="left")
        ttk.Button(controls, text="Download selected", command=self.download).pack(side="left")
        ttk.Button(controls, text="Delete selected…", command=self.prepare_delete).pack(side="left")
        ttk.Button(controls, text="Open saved print log", command=self.view_log).pack(side="left")
        folder = ttk.Frame(root, padding=10)
        folder.pack(fill="x")
        ttk.Label(folder, text="Save folder:").pack(side="left")
        ttk.Entry(folder, textvariable=self.output).pack(side="left", fill="x", expand=True)
        live = ttk.LabelFrame(root, text="Direct sensor data", padding=10)
        live.pack(fill="x", padx=10, pady=(0, 10))
        ttk.Checkbutton(live, text="Live sensors (bypass flash)", variable=self.live_enabled,
                        command=lambda: self.actions.put(("live", self.live_enabled.get()))).pack(anchor="w")
        ttk.Label(live, textvariable=self.live_text, font="TkFixedFont", justify="left").pack(anchor="w")
        self.file_tabs = ttk.Notebook(root)
        ordinary, launches = ttk.Frame(self.file_tabs), ttk.Frame(self.file_tabs)
        self.file_tabs.add(ordinary, text="Rolling history")
        self.file_tabs.add(launches, text="Protected launch / drop files")
        self.file_tabs.pack(fill="x", padx=10)
        self.sessions = ttk.Treeview(ordinary, columns=("id", "generation", "length", "incomplete"), show="headings", height=6)
        for column in self.sessions["columns"]:
            self.sessions.heading(column, text=column.capitalize())
        self.sessions.pack(fill="x")
        self.launches = ttk.Treeview(launches, columns=("id", "generation", "length", "reasons", "pre", "post", "state", "protected"),
                                     show="headings", height=6)
        for column, label, width in (("id", "ID", 55), ("generation", "Generation", 75),
                                    ("length", "Export bytes", 100), ("reasons", "Trigger", 180),
                                    ("pre", "Before", 75), ("post", "After / 300 s", 100),
                                    ("state", "Capture", 100), ("protected", "Protected", 75)):
            self.launches.heading(column, text=label)
            self.launches.column(column, width=width)
        self.launches.pack(fill="x")
        ttk.Label(root, text="USB prints — saved automatically while connected", padding=10).pack(anchor="w")
        frame = ttk.Frame(root)
        frame.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.console = tk.Text(frame, wrap="word", state="disabled")
        scroll = ttk.Scrollbar(frame, command=self.console.yview)
        self.console.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.console.pack(fill="both", expand=True)
        self.refresh()
        self.worker_thread = threading.Thread(target=self.worker, daemon=True)
        self.worker_thread.start()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(50, self.pump)

    def refresh(self):
        try:
            from serial.tools import list_ports
            values = [p.device for p in list_ports.comports()]
            self.ports["values"] = values
            if values and not self.port.get():
                self.port.set(values[0])
        except ImportError:
            self.state.set("Install client/requirements.txt to connect")

    def download(self):
        launch = self.file_tabs.index("current") == 1
        table = self.launches if launch else self.sessions
        selected = table.selection()
        if selected:
            sid = int(table.item(selected[0], "values")[0])
            self.actions.put(("download", (sid, self.output.get(), launch)))

    def view_log(self):
        path = filedialog.askopenfilename(initialdir=self.output.get(), filetypes=[("Print logs", "*.log"), ("All files", "*")])
        if path:
            self.append(f"\n--- Saved log: {path} ---\n" + Path(path).read_text(encoding="utf-8", errors="replace"))

    def prepare_delete(self):
        launch = self.file_tabs.index("current") == 1
        table = self.launches if launch else self.sessions
        selected = table.selection()
        if selected:
            sid = int(table.item(selected[0], "values")[0])
            self.actions.put(("prepare_delete", (sid, launch)))

    def append(self, value):
        self.console.configure(state="normal")
        self.console.insert("end", value)
        # Keep the widget bounded; full output remains on disk.
        lines = int(self.console.index("end-1c").split(".")[0])
        if lines > 3000:
            self.console.delete("1.0", f"{lines - 3000}.0")
        self.console.see("end")
        self.console.configure(state="disabled")

    def pump(self):
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "sessions":
                selected = self.sessions.selection()
                old_id = self.sessions.item(selected[0], "values")[0] if selected else None
                for item in self.sessions.get_children():
                    self.sessions.delete(item)
                for s in value:
                    item = self.sessions.insert("", "end", values=(s.id, s.generation, s.length, s.incomplete))
                    if str(s.id) == str(old_id):
                        self.sessions.selection_set(item)
            elif kind == "status":
                capture = (f"  Capturing: {value.get('remaining_s', '0')} s remaining"
                           if value.get("state") == "CAPTURING" else
                           "  Launch capture armed" if value.get("armed") == "1" else
                           "  Launch capture unavailable: export/delete saved launches or wait for readiness"
                           if value.get("events") == "1" else "")
                self.device_state.set(
                    f"{value.get('state', 'Unknown')}  Session: {value.get('active', '0')}  "
                    f"Durable records: {value.get('durable', '0')}  "
                    f"Flash data: {int(value.get('phys_bytes', '0')) / 1024:.1f} / 4096 KiB  "
                    f"Storage drops: {value.get('storage_drop', '0')}{capture}")
            elif kind == "launches":
                selected = self.launches.selection()
                old_id = self.launches.item(selected[0], "values")[0] if selected else None
                for item in self.launches.get_children():
                    self.launches.delete(item)
                for entry in value:
                    reasons = "+".join(name for bit, name in ((1, "Spike"), (2, "Free-fall"), (4, "Altitude")) if entry.reasons & bit)
                    state = "Capturing" if entry.state == "CAPTURING" else "Incomplete" if entry.incomplete else "Complete"
                    item = self.launches.insert("", "end", values=(entry.id, entry.generation, entry.length, reasons,
                        f"{entry.pre_ms / 1000:.2f} s", f"{entry.post_ms / 1000:.2f} s", state, "Yes"))
                    if str(entry.id) == str(old_id):
                        self.launches.selection_set(item)
            elif kind == "confirm_delete":
                sid, generation, token, launch = value
                if messagebox.askyesno("Delete flash session",
                        f"Permanently delete {'PROTECTED launch/drop file' if launch else 'session'} {sid}?\n\n"
                        "This erases its data from the board and cannot be undone.\n"
                        "Confirm within 30 seconds; saved downloads remain on this computer.",
                        icon="warning"):
                    self.actions.put(("confirm_delete", (sid, generation, token)))
            elif kind == "error":
                self.state.set(value)
                messagebox.showerror("USB client", value)
            elif kind == "state":
                self.state.set(value)
            elif kind == "live":
                self.live_text.set(value)
            elif kind == "live_disabled":
                self.live_enabled.set(False)
                self.live_text.set(value)
            else:
                self.append(value)
        if not self.shutdown.is_set():
            self.root.after(50, self.pump)

    def worker(self):
        client, raw, meta, live_file = None, None, None, None
        cursor = 0
        last_poll = 0
        last_live = 0
        live_enabled = False
        last_status = last_operation = 0
        last_events = 0
        status_identity = None
        operation = None

        def disconnect():
            nonlocal client, raw, meta, live_file, operation
            if client:
                try:
                    client.close()
                finally:
                    client = None
            for f in (raw, meta, live_file):
                if f:
                    f.close()
            raw = meta = live_file = None
            operation = None

        try:
            while not self.shutdown.is_set():
                try:
                    action, value = self.actions.get(timeout=0.05)
                except queue.Empty:
                    action, value = None, None
                try:
                    if action == "connect":
                        disconnect()
                        port, destination = value
                        client = open_client(port)
                        folder = Path(destination)
                        folder.mkdir(parents=True, exist_ok=True)
                        filename = folder / time.strftime("usb-prints-%Y%m%d-%H%M%S.log")
                        raw = filename.open("ab")
                        meta = filename.with_suffix(".log.jsonl").open("a", encoding="utf-8")
                        live_file = filename.with_name(filename.stem + "-sensors.jsonl").open("a", encoding="utf-8")
                        cursor = 0
                        last_live = 0
                        last_status = 0
                        last_events = 0
                        status_identity = None
                        self.events.put(("state", f"Connected: {port}; saving prints to {filename}"))
                        self.events.put(("text", json.dumps(client.status(), indent=2) + "\n"))
                    elif action == "disconnect":
                        disconnect()
                        self.events.put(("state", "Disconnected"))
                        self.events.put(("status", {"state": "Disconnected"}))
                        self.events.put(("live", "Disconnected"))
                    elif action == "live":
                        live_enabled = value
                        last_live = 0
                        self.events.put(("live", "Waiting for sensor data" if value else "Live sensors disabled"))
                    elif action:
                        if client is None:
                            raise RuntimeError("Connect a USB CDC port first")
                        if action == "LIST":
                            self.events.put(("sessions", client.sessions()))
                        elif action == "STATUS":
                            self.events.put(("status", client.status()))
                        elif action == "download":
                            sid, folder, launch = value
                            path = export_session(client, sid, Path(folder), launch=launch)
                            self.events.put(("text", f"Saved {path}\n"))
                            self.events.put(("sessions", client.sessions()))
                        elif action == "prepare_delete":
                            sid, launch = value
                            client.command("STOP")
                            fields = client.command(f"ERASE_PREPARE {sid}").split()
                            if len(fields) != 5 or fields[:2] != ["OK", "ERASE_PREPARE"] or int(fields[2]) != sid:
                                raise RuntimeError("Unexpected erase preparation response")
                            self.events.put(("confirm_delete", (sid, int(fields[3]), int(fields[4]), launch)))
                        elif action == "confirm_delete":
                            sid, generation, token = value
                            fields = client.command(f"ERASE_CONFIRM {sid} {generation} {token}").split()
                            if len(fields) != 3 or fields[:2] != ["OK", "ERASE_CONFIRM"]:
                                raise RuntimeError("Unexpected erase confirmation response")
                            operation = int(fields[2]); last_operation = 0
                            self.events.put(("text", f"Deleting session {sid}…\n"))
                        else:
                            self.events.put(("text", client.command(action) + "\n"))
                    if client and time.monotonic() - last_status >= 1:
                        status = client.status()
                        self.events.put(("status", status))
                        identity = (status.get("state"), status.get("active"))
                        if status.get("metadata") == "1" and identity != status_identity:
                            self.events.put(("sessions", client.sessions()))
                        if status.get("events") == "1" and time.monotonic() - last_events >= 1:
                            self.events.put(("launches", client.events()))
                            last_events = time.monotonic()
                        status_identity = identity
                        last_status = time.monotonic()
                    if client and operation is not None and time.monotonic() - last_operation >= 0.25:
                        result = client.operation_status(operation)
                        self.events.put(("text", f"Deletion {operation}: {result['state']} "
                                                 f"{result['done']}/{result['total']} sectors\n"))
                        if result["state"] in ("DONE", "FAILED"):
                            if result["state"] == "FAILED":
                                self.events.put(("error", f"Flash deletion failed: {result}"))
                            operation = None
                            self.events.put(("sessions", client.sessions()))
                        last_operation = time.monotonic()
                    if client and time.monotonic() - last_poll >= 0.1:
                        start, end, lost, data = client.logs(cursor)
                        if data or start != cursor:
                            meta.write(json.dumps({"unix_time": time.time(), "cursor": start,
                                                   "file_offset": raw.tell(), "length": len(data),
                                                   "overwritten": lost, "gap": (start - cursor) & U32_MAX}) + "\n")
                            raw.write(data)
                            raw.flush(); meta.flush()
                            self.events.put(("text", data.decode("utf-8", errors="replace")))
                        cursor = (start + len(data)) & U32_MAX
                        last_poll = time.monotonic() if cursor == end else 0
                    if client and live_enabled and time.monotonic() - last_live >= 0.2:
                        try:
                            sample = client.live()
                        except DeviceError as error:
                            if error.code == "NOT_READY":
                                self.events.put(("live", "Sensors initializing"))
                            else:
                                live_enabled = False
                                self.events.put(("live_disabled", f"Live sensors unavailable: {error}"))
                        else:
                            live_file.write(json.dumps({"unix_time": time.time(), **sample}) + "\n")
                            live_file.flush()
                            p = sample["processed"]
                            flags = p["flags"]
                            def vector(values, valid):
                                return ", ".join(f"{v:9.3f}" for v in values) if valid else "invalid"
                            pressure = f"{p['pressure_pa']:.1f}" if flags & 4 else "invalid"
                            temperature = f"{p['temperature_c']:.2f}" if flags & 8 else "invalid"
                            altitude = f"{p['altitude_m']:.2f} m" if flags & 256 else "waiting for valid pressure"
                            stale = " STALE" if sample["sample_age_ms"] > 1000 else ""
                            self.events.put(("live",
                                f"Sample {sample['sequence']}  age {sample['sample_age_ms']} ms{stale}  "
                                f"flags=0x{flags:03X} errors=0x{p['errors']:02X}\n"
                                f"Accel m/s²: {vector(p['acceleration_m_s2'], flags & 1)}\n"
                                f"Gyro rad/s: {vector(p['angular_rate_rad_s'], flags & 2)}\n"
                                f"Altitude: {altitude} (relative to connection start)\n"
                                f"Pressure: {pressure} Pa  Temperature: {temperature} °C  "
                                f"barometer age: {p['baro_age_us'] / 1000:.1f} ms"))
                        last_live = time.monotonic()
                except Exception as error:
                    self.events.put(("error", str(error)))
                    if client and (client.broken or raw is None):
                        disconnect()
        finally:
            disconnect()

    def connect(self):
        self.actions.put(("connect", (self.port.get(), self.output.get())))

    def close(self):
        self.shutdown.set()
        self.state.set("Closing USB connection…")
        self.finish_close()

    def finish_close(self):
        if self.worker_thread.is_alive():
            self.root.after(100, self.finish_close)
        else:
            self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = App(root)
    root.mainloop()
