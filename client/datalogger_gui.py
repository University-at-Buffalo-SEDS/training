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
        root.title("Datalogger USB")
        root.geometry("1000x720")
        self.actions, self.events = queue.Queue(), queue.Queue()
        self.shutdown = threading.Event()
        self.port = tk.StringVar()
        self.state = tk.StringVar(value="Disconnected")
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
        controls = ttk.Frame(root, padding=10)
        controls.pack(fill="x")
        for label, action in (("Status", "STATUS"), ("Start", "START"), ("Stop", "STOP"), ("List sessions", "LIST")):
            ttk.Button(controls, text=label, command=lambda a=action: self.actions.put((a, None))).pack(side="left")
        ttk.Button(controls, text="Download selected", command=self.download).pack(side="left")
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
        self.sessions = ttk.Treeview(root, columns=("id", "generation", "length", "incomplete"), show="headings", height=7)
        for column in self.sessions["columns"]:
            self.sessions.heading(column, text=column.capitalize())
        self.sessions.pack(fill="x", padx=10)
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
        selected = self.sessions.selection()
        if selected:
            sid = int(self.sessions.item(selected[0], "values")[0])
            self.actions.put(("download", (sid, self.output.get())))

    def view_log(self):
        path = filedialog.askopenfilename(initialdir=self.output.get(), filetypes=[("Print logs", "*.log"), ("All files", "*")])
        if path:
            self.append(f"\n--- Saved log: {path} ---\n" + Path(path).read_text(encoding="utf-8", errors="replace"))

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
                for item in self.sessions.get_children():
                    self.sessions.delete(item)
                for s in value:
                    self.sessions.insert("", "end", values=(s.id, s.generation, s.length, s.incomplete))
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

        def disconnect():
            nonlocal client, raw, meta, live_file
            if client:
                try:
                    client.close()
                finally:
                    client = None
            for f in (raw, meta, live_file):
                if f:
                    f.close()
            raw = meta = live_file = None

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
                        self.events.put(("state", f"Connected: {port}; saving prints to {filename}"))
                        self.events.put(("text", json.dumps(client.status(), indent=2) + "\n"))
                    elif action == "disconnect":
                        disconnect()
                        self.events.put(("state", "Disconnected"))
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
                        elif action == "download":
                            sid, folder = value
                            path = export_session(client, sid, Path(folder))
                            self.events.put(("text", f"Saved {path}\n"))
                        else:
                            self.events.put(("text", client.command(action) + "\n"))
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
