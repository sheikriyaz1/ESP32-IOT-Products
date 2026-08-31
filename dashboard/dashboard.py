import json
import os
import tkinter as tk
from collections import deque
from datetime import datetime
import paho.mqtt.client as mqtt


# ============================================================
# MQTT CONFIGURATION
# ============================================================

MQTT_BROKER = "10.72.217.50"
MQTT_PORT = 1883
MQTT_TOPIC = "device/+/telemetry"

MAX_POINTS = 60
MAX_EVENTS = 20

# Persistent history file
HISTORY_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "energy_event_history.json"
)


# ============================================================
# DASHBOARD
# ============================================================

class EnergyDashboard:

    def __init__(self, root):

        self.root = root

        self.root.title(
            "ESP32-C3 Smart Energy Dashboard"
        )

        self.root.geometry(
            "1150x1000"
        )

        self.root.minsize(
            1000,
            850
        )

        self.device_id = None
        self.last_state_time = "--"

        # ====================================================
        # LIVE DATA
        # ====================================================

        self.voltage = deque(
            maxlen=MAX_POINTS
        )

        self.current = deque(
            maxlen=MAX_POINTS
        )

        self.power = deque(
            maxlen=MAX_POINTS
        )

        # ====================================================
        # EVENT HISTORY
        # ====================================================

        self.event_history = deque(
            maxlen=MAX_EVENTS
        )

        self.last_trip = None
        self.last_alarm = None

        # Load previously saved history
        self.load_history()

        # ====================================================
        # TITLE
        # ====================================================

        tk.Label(
            root,
            text="ESP32-C3 SMART ENERGY DASHBOARD",
            font=("Arial", 25, "bold")
        ).pack(
            pady=(15, 3)
        )

        self.status = tk.Label(
            root,
            text="MQTT: CONNECTING...",
            font=("Arial", 12, "bold")
        )

        self.status.pack()

        # ====================================================
        # SAFETY STATUS
        # ====================================================

        self.alarm_frame = tk.Frame(
            root,
            bd=3,
            relief="groove",
            padx=20,
            pady=8
        )

        self.alarm_frame.pack(
            fill="x",
            padx=25,
            pady=10
        )

        tk.Label(
            self.alarm_frame,
            text="SAFETY STATUS",
            font=("Arial", 13, "bold")
        ).pack()

        self.alarm_status = tk.Label(
            self.alarm_frame,
            text="WAITING FOR DATA",
            font=("Arial", 19, "bold")
        )

        self.alarm_status.pack(
            pady=3
        )

        self.trip_status = tk.Label(
            self.alarm_frame,
            text="Trips: --",
            font=("Arial", 11)
        )

        self.trip_status.pack()

        # ====================================================
        # LIVE VALUES
        # ====================================================

        value_frame = tk.Frame(
            root
        )

        value_frame.pack(
            pady=5
        )

        self.values = {}

        fields = [
            ("Voltage", "V", "v"),
            ("Current", "A", "i"),
            ("Power", "W", "p"),
            ("Power Factor", "", "pf"),
            ("Energy", "kWh", "e_kwh"),
            ("Daily Energy", "kWh", "e_day"),
            ("Alarm", "", "alarm"),
            ("Trips", "", "trips"),
        ]

        for col, (name, unit, key) in enumerate(fields):

            card = tk.Frame(
                value_frame,
                bd=2,
                relief="groove",
                padx=9,
                pady=6
            )

            card.grid(
                row=0,
                column=col,
                padx=3
            )

            tk.Label(
                card,
                text=name,
                font=("Arial", 9, "bold")
            ).pack()

            value = tk.Label(
                card,
                text="--",
                font=("Arial", 15, "bold")
            )

            value.pack()

            tk.Label(
                card,
                text=unit,
                font=("Arial", 8)
            ).pack()

            self.values[key] = value

        # ====================================================
        # DEVICE STATE
        # ====================================================

        state_frame = tk.LabelFrame(
            root,
            text="DEVICE STATE",
            font=("Arial", 13, "bold"),
            padx=15,
            pady=8
        )

        state_frame.pack(
            fill="x",
            padx=25,
            pady=8
        )

        self.state_values = {}

        state_fields = [
            ("Voltage", "v", "V"),
            ("Current", "i", "A"),
            ("Power", "p", "W"),
            ("Power Factor", "pf", ""),
            ("Energy", "e_kwh", "kWh"),
            ("Daily Energy", "e_day", "kWh"),
            ("Alarm", "alarm", ""),
            ("Trips", "trips", ""),
        ]

        for col, (name, key, unit) in enumerate(state_fields):

            frame = tk.Frame(
                state_frame,
                padx=8
            )

            frame.grid(
                row=0,
                column=col
            )

            tk.Label(
                frame,
                text=name,
                font=("Arial", 9, "bold")
            ).pack()

            label = tk.Label(
                frame,
                text="--",
                font=("Arial", 11, "bold")
            )

            label.pack()

            if unit:

                tk.Label(
                    frame,
                    text=unit,
                    font=("Arial", 8)
                ).pack()

            self.state_values[key] = label

        self.state_update_label = tk.Label(
            state_frame,
            text="Last state update: --",
            font=("Arial", 9)
        )

        self.state_update_label.grid(
            row=1,
            column=0,
            columnspan=8,
            pady=(5, 0)
        )

        # ====================================================
        # COMMAND CONTROL
        # ====================================================

        control_frame = tk.LabelFrame(
            root,
            text="ENERGY COMMAND CONTROL",
            font=("Arial", 13, "bold"),
            padx=15,
            pady=8
        )

        control_frame.pack(
            fill="x",
            padx=25,
            pady=8
        )

        tk.Label(
            control_frame,
            text="Over-Power Limit:"
        ).grid(
            row=0,
            column=0,
            padx=8,
            pady=5
        )

        self.limit_entry = tk.Entry(
            control_frame,
            width=12,
            font=("Arial", 11)
        )

        self.limit_entry.insert(
            0,
            "4000"
        )

        self.limit_entry.grid(
            row=0,
            column=1
        )

        tk.Label(
            control_frame,
            text="W"
        ).grid(
            row=0,
            column=2
        )

        self.set_limit_button = tk.Button(
            control_frame,
            text="SET LIMIT",
            font=("Arial", 10, "bold"),
            command=self.set_limit
        )

        self.set_limit_button.grid(
            row=0,
            column=3,
            padx=15
        )

        self.get_state_button = tk.Button(
            control_frame,
            text="GET STATE",
            font=("Arial", 10, "bold"),
            command=self.get_state
        )

        self.get_state_button.grid(
            row=0,
            column=4,
            padx=10
        )

        self.command_status = tk.Label(
            control_frame,
            text="Command: waiting",
            font=("Arial", 10)
        )

        self.command_status.grid(
            row=1,
            column=0,
            columnspan=5,
            sticky="w",
            padx=8
        )

        # ====================================================
        # ALARM & TRIP HISTORY
        # ====================================================

        history_frame = tk.LabelFrame(
            root,
            text="ALARM & TRIP HISTORY",
            font=("Arial", 13, "bold"),
            padx=10,
            pady=6
        )

        history_frame.pack(
            fill="x",
            padx=25,
            pady=8
        )

        history_scroll = tk.Scrollbar(
            history_frame
        )

        history_scroll.pack(
            side="right",
            fill="y"
        )

        self.history_list = tk.Listbox(
            history_frame,
            height=6,
            font=("Consolas", 10),
            yscrollcommand=history_scroll.set
        )

        self.history_list.pack(
            fill="x",
            expand=True
        )

        history_scroll.config(
            command=self.history_list.yview
        )

        # Display loaded history
        self.refresh_history_list()

        # ====================================================
        # GRAPH AREA
        # ====================================================

        graph_frame = tk.Frame(
            root
        )

        graph_frame.pack(
            fill="both",
            expand=True,
            padx=15
        )

        self.voltage_canvas = self.create_graph(
            graph_frame,
            "Voltage"
        )

        self.current_canvas = self.create_graph(
            graph_frame,
            "Current"
        )

        self.power_canvas = self.create_graph(
            graph_frame,
            "Power"
        )

        # ====================================================
        # INFORMATION
        # ====================================================

        self.topic_label = tk.Label(
            root,
            text="Waiting for telemetry...",
            font=("Arial", 9)
        )

        self.topic_label.pack()

        self.device_label = tk.Label(
            root,
            text="Device: --",
            font=("Arial", 9)
        )

        self.device_label.pack()

        self.points_label = tk.Label(
            root,
            text="Samples: 0",
            font=("Arial", 9)
        )

        self.points_label.pack(
            pady=(0, 6)
        )

        # ====================================================
        # MQTT
        # ====================================================

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="ESP32_Energy_Dashboard"
        )

        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect

        try:

            self.client.connect(
                MQTT_BROKER,
                MQTT_PORT,
                60
            )

            self.client.loop_start()

        except Exception as e:

            self.status.config(
                text=f"MQTT ERROR: {e}"
            )

        self.root.protocol(
            "WM_DELETE_WINDOW",
            self.close
        )

    # ========================================================
    # LOAD HISTORY
    # ========================================================

    def load_history(self):

        if not os.path.exists(HISTORY_FILE):

            return

        try:

            with open(
                HISTORY_FILE,
                "r",
                encoding="utf-8"
            ) as file:

                data = json.load(file)

            if isinstance(data, list):

                for event in data[-MAX_EVENTS:]:

                    if isinstance(event, dict):

                        self.event_history.append(
                            event
                        )

            print(
                f"Loaded {len(self.event_history)} "
                f"saved events."
            )

        except Exception as e:

            print(
                "Could not load event history:",
                e
            )

    # ========================================================
    # SAVE HISTORY
    # ========================================================

    def save_history(self):

        try:

            data = list(
                self.event_history
            )

            with open(
                HISTORY_FILE,
                "w",
                encoding="utf-8"
            ) as file:

                json.dump(
                    data,
                    file,
                    indent=2
                )

        except Exception as e:

            print(
                "Could not save event history:",
                e
            )

    # ========================================================
    # REFRESH HISTORY LIST
    # ========================================================

    def refresh_history_list(self):

        self.history_list.delete(
            0,
            tk.END
        )

        for event in reversed(
            self.event_history
        ):

            timestamp = event.get(
                "time",
                "--"
            )

            event_name = event.get(
                "event",
                "UNKNOWN"
            )

            trips = event.get(
                "trips",
                "--"
            )

            entry = (
                f"{timestamp:<10}"
                f"{event_name:<20}"
                f"Trip: {trips}"
            )

            self.history_list.insert(
                tk.END,
                entry
            )

    # ========================================================
    # GRAPH CREATION
    # ========================================================

    def create_graph(
        self,
        parent,
        title
    ):

        canvas = tk.Canvas(
            parent,
            height=105,
            bg="white",
            highlightthickness=1
        )

        canvas.pack(
            fill="x",
            pady=4
        )

        canvas.create_text(
            10,
            8,
            anchor="nw",
            text=title,
            font=("Arial", 10, "bold")
        )

        return canvas

    # ========================================================
    # MQTT CONNECT
    # ========================================================

    def on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties
    ):

        if reason_code == 0:

            client.subscribe(
                MQTT_TOPIC
            )

            self.root.after(
                0,
                lambda: self.status.config(
                    text="MQTT: CONNECTED"
                )
            )

        else:

            self.root.after(
                0,
                lambda: self.status.config(
                    text=(
                        f"MQTT CONNECTION FAILED: "
                        f"{reason_code}"
                    )
                )
            )

    # ========================================================
    # MQTT DISCONNECT
    # ========================================================

    def on_disconnect(
        self,
        client,
        userdata,
        disconnect_flags,
        reason_code,
        properties=None
    ):

        self.root.after(
            0,
            lambda: self.status.config(
                text="MQTT: DISCONNECTED"
            )
        )

    # ========================================================
    # MQTT MESSAGE
    # ========================================================

    def on_message(
        self,
        client,
        userdata,
        message
    ):

        try:

            data = json.loads(
                message.payload.decode(
                    "utf-8"
                )
            )

            self.root.after(
                0,
                self.update_dashboard,
                data,
                message.topic
            )

        except Exception as e:

            print(
                "Invalid MQTT message:",
                e
            )

    # ========================================================
    # UPDATE DASHBOARD
    # ========================================================

    def update_dashboard(
        self,
        data,
        topic
    ):

        try:

            parts = topic.split("/")

            if len(parts) >= 2:

                self.device_id = parts[1]

                self.device_label.config(
                    text=(
                        f"Device: "
                        f"{self.device_id}"
                    )
                )

            # ------------------------------------------------
            # GET STATE RESPONSE
            # ------------------------------------------------

            is_state = (
                data.get("uc")
                ==
                "energy.get_state"
            )

            # ------------------------------------------------
            # Voltage
            # ------------------------------------------------

            if "v" in data:

                v = float(
                    data["v"]
                )

                self.values["v"].config(
                    text=f"{v:.2f}"
                )

                self.voltage.append(v)

            # ------------------------------------------------
            # Current
            # ------------------------------------------------

            if "i" in data:

                i = float(
                    data["i"]
                )

                self.values["i"].config(
                    text=f"{i:.3f}"
                )

                self.current.append(i)

            # ------------------------------------------------
            # Power
            # ------------------------------------------------

            if "p" in data:

                p = float(
                    data["p"]
                )

                self.values["p"].config(
                    text=f"{p:.1f}"
                )

                self.power.append(p)

            # ------------------------------------------------
            # Power Factor
            # ------------------------------------------------

            if "pf" in data:

                pf = float(
                    data["pf"]
                )

                self.values["pf"].config(
                    text=f"{pf:.3f}"
                )

            # ------------------------------------------------
            # Energy
            # ------------------------------------------------

            if "e_kwh" in data:

                energy = float(
                    data["e_kwh"]
                )

                self.values["e_kwh"].config(
                    text=f"{energy:.5f}"
                )

            # ------------------------------------------------
            # Daily Energy
            # ------------------------------------------------

            if "e_day" in data:

                daily = float(
                    data["e_day"]
                )

                self.values["e_day"].config(
                    text=f"{daily:.5f}"
                )

            # ------------------------------------------------
            # Alarm
            # ------------------------------------------------

            if "alarm" in data:

                alarm = int(
                    data["alarm"]
                )

                self.values["alarm"].config(
                    text=(
                        "ON"
                        if alarm
                        else "OFF"
                    )
                )

                if alarm:

                    self.alarm_status.config(
                        text="ALARM ACTIVE"
                    )

                    self.alarm_frame.config(
                        bd=5
                    )

                else:

                    self.alarm_status.config(
                        text="SYSTEM NORMAL"
                    )

                    self.alarm_frame.config(
                        bd=3
                    )

            # ------------------------------------------------
            # Trips
            # ------------------------------------------------

            if "trips" in data:

                trips = int(
                    data["trips"]
                )

                self.values["trips"].config(
                    text=str(trips)
                )

                self.trip_status.config(
                    text=f"Trips: {trips}"
                )

            # ------------------------------------------------
            # DEVICE STATE
            # ------------------------------------------------

            if is_state:

                if "v" in data:

                    self.state_values["v"].config(
                        text=(
                            f"{float(data['v']):.2f}"
                        )
                    )

                if "i" in data:

                    self.state_values["i"].config(
                        text=(
                            f"{float(data['i']):.3f}"
                        )
                    )

                if "p" in data:

                    self.state_values["p"].config(
                        text=(
                            f"{float(data['p']):.1f}"
                        )
                    )

                if "pf" in data:

                    self.state_values["pf"].config(
                        text=(
                            f"{float(data['pf']):.3f}"
                        )
                    )

                if "e_kwh" in data:

                    self.state_values["e_kwh"].config(
                        text=(
                            f"{float(data['e_kwh']):.5f}"
                        )
                    )

                if "e_day" in data:

                    self.state_values["e_day"].config(
                        text=(
                            f"{float(data['e_day']):.5f}"
                        )
                    )

                if "alarm" in data:

                    alarm_text = (
                        "ON"
                        if int(data["alarm"])
                        else "OFF"
                    )

                    self.state_values["alarm"].config(
                        text=alarm_text
                    )

                if "trips" in data:

                    self.state_values["trips"].config(
                        text=str(
                            int(data["trips"])
                        )
                    )

                self.last_state_time = (
                    datetime.now().strftime(
                        "%H:%M:%S"
                    )
                )

                self.state_update_label.config(
                    text=(
                        "Last state update: "
                        + self.last_state_time
                    )
                )

                self.command_status.config(
                    text=(
                        "Command: "
                        "GET STATE response received"
                    )
                )

            # ------------------------------------------------
            # EVENT HISTORY
            # ------------------------------------------------

            self.update_event_history(
                data
            )

            # ------------------------------------------------
            # INFORMATION
            # ------------------------------------------------

            self.topic_label.config(
                text=(
                    f"MQTT Topic: "
                    f"{topic}"
                )
            )

            self.points_label.config(
                text=(
                    f"Samples: "
                    f"{len(self.power)} / "
                    f"{MAX_POINTS}"
                )
            )

            # ------------------------------------------------
            # GRAPHS
            # ------------------------------------------------

            self.draw_graph(
                self.voltage_canvas,
                self.voltage,
                "Voltage"
            )

            self.draw_graph(
                self.current_canvas,
                self.current,
                "Current"
            )

            self.draw_graph(
                self.power_canvas,
                self.power,
                "Power"
            )

        except Exception as e:

            print(
                "Dashboard update error:",
                e
            )

    # ========================================================
    # EVENT HISTORY PROCESSING
    # ========================================================

    def update_event_history(
        self,
        data
    ):

        if (
            "alarm" not in data
            or
            "trips" not in data
        ):

            return

        try:

            alarm = int(
                data["alarm"]
            )

            trips = int(
                data["trips"]
            )

        except (
            ValueError,
            TypeError
        ):

            return

        now = datetime.now().strftime(
            "%H:%M:%S"
        )

        # ----------------------------------------------------
        # First live message after dashboard startup
        # ----------------------------------------------------

        if self.last_alarm is None:

            self.last_alarm = alarm
            self.last_trip = trips

            # Don't create a duplicate INITIAL STATUS
            # if saved history already exists.

            if len(self.event_history) == 0:

                self.add_event(
                    now,
                    "INITIAL STATUS",
                    trips
                )

            return

        # ----------------------------------------------------
        # Trip changed
        # ----------------------------------------------------

        if trips != self.last_trip:

            self.add_event(
                now,
                "TRIP / ALARM",
                trips
            )

            self.last_trip = trips

        # ----------------------------------------------------
        # Alarm changed
        # ----------------------------------------------------

        if alarm != self.last_alarm:

            if alarm:

                event = "ALARM ACTIVE"

            else:

                event = "ALARM CLEARED"

            self.add_event(
                now,
                event,
                trips
            )

            self.last_alarm = alarm

    # ========================================================
    # ADD EVENT
    # ========================================================

    def add_event(
        self,
        timestamp,
        event,
        trips
    ):

        record = {
            "time": timestamp,
            "event": event,
            "trips": trips
        }

        self.event_history.append(
            record
        )

        self.refresh_history_list()

        # Save immediately
        self.save_history()

    # ========================================================
    # SET LIMIT
    # ========================================================

    def set_limit(self):

        if self.device_id is None:

            self.command_status.config(
                text=(
                    "Command: "
                    "waiting for device..."
                )
            )

            return

        try:

            limit = float(
                self.limit_entry.get()
            )

            if limit <= 0:

                self.command_status.config(
                    text=(
                        "Command: "
                        "limit must be greater than 0"
                    )
                )

                return

            payload = {
                "uc": "energy.set_limits",
                "over_w": limit
            }

            self.publish_command(
                payload,
                "SET LIMIT"
            )

        except ValueError:

            self.command_status.config(
                text=(
                    "Command: "
                    "invalid power limit"
                )
            )

    # ========================================================
    # GET STATE
    # ========================================================

    def get_state(self):

        if self.device_id is None:

            self.command_status.config(
                text=(
                    "Command: "
                    "waiting for device..."
                )
            )

            return

        payload = {
            "uc": "energy.get_state"
        }

        self.publish_command(
            payload,
            "GET STATE"
        )

    # ========================================================
    # PUBLISH COMMAND
    # ========================================================

    def publish_command(
        self,
        payload,
        command_name
    ):

        try:

            topic = (
                f"device/"
                f"{self.device_id}"
                f"/command"
            )

            message = json.dumps(
                payload,
                separators=(",", ":")
            )

            result = self.client.publish(
                topic,
                message,
                qos=0
            )

            if (
                result.rc
                ==
                mqtt.MQTT_ERR_SUCCESS
            ):

                self.command_status.config(
                    text=(
                        f"Command sent: "
                        f"{command_name}"
                    )
                )

                print(
                    f"MQTT COMMAND → {topic}"
                )

                print(
                    message
                )

            else:

                self.command_status.config(
                    text=(
                        f"Command failed: "
                        f"{result.rc}"
                    )
                )

        except Exception as e:

            self.command_status.config(
                text=(
                    f"Command error: "
                    f"{e}"
                )
            )

            print(
                "Command error:",
                e
            )

    # ========================================================
    # DRAW GRAPH
    # ========================================================

    def draw_graph(
        self,
        canvas,
        values,
        label
    ):

        canvas.delete(
            "all"
        )

        width = canvas.winfo_width()

        if width < 100:

            width = 100

        height = canvas.winfo_height()

        if height < 50:

            height = 50

        canvas.create_text(
            10,
            8,
            anchor="nw",
            text=label,
            font=("Arial", 10, "bold")
        )

        if len(values) < 2:

            canvas.create_text(
                width // 2,
                height // 2,
                text="Waiting for data..."
            )

            return

        data = list(
            values
        )

        minimum = min(
            data
        )

        maximum = max(
            data
        )

        if maximum == minimum:

            minimum -= 1
            maximum += 1

        margin_x = 40
        margin_y = 23

        graph_width = (
            width
            -
            2 * margin_x
        )

        graph_height = (
            height
            -
            2 * margin_y
        )

        canvas.create_line(
            margin_x,
            margin_y,
            margin_x,
            height - margin_y
        )

        canvas.create_line(
            margin_x,
            height - margin_y,
            width - margin_x,
            height - margin_y
        )

        canvas.create_text(
            5,
            margin_y,
            anchor="w",
            text=f"{maximum:.1f}"
        )

        canvas.create_text(
            5,
            height - margin_y,
            anchor="w",
            text=f"{minimum:.1f}"
        )

        points = []

        for index, value in enumerate(
            data
        ):

            x = (
                margin_x
                +
                index
                *
                graph_width
                /
                (len(data) - 1)
            )

            y = (
                height
                -
                margin_y
                -
                (
                    (
                        value
                        -
                        minimum
                    )
                    /
                    (
                        maximum
                        -
                        minimum
                    )
                    *
                    graph_height
                )
            )

            points.extend(
                [x, y]
            )

        if len(points) >= 4:

            canvas.create_line(
                *points,
                width=2,
                smooth=True
            )

    # ========================================================
    # CLOSE
    # ========================================================

    def close(self):

        try:

            self.save_history()

            self.client.loop_stop()

            self.client.disconnect()

        except Exception:
            pass

        self.root.destroy()


# ============================================================
# START DASHBOARD
# ============================================================

root = tk.Tk()

app = EnergyDashboard(
    root
)

root.mainloop()