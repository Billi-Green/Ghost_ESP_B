import sys
import json
import queue
import serial
import threading
import time
from datetime import datetime
from serial.tools import list_ports
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                             QHBoxLayout, QComboBox, QPushButton, QLabel, QTextEdit,
                             QTabWidget, QGroupBox, QGridLayout, QLineEdit, QMessageBox,
                             QSplitter, QInputDialog, QSpinBox, QFormLayout, QStyle, QFileDialog, QCheckBox)
from PyQt6.QtCore import Qt, pyqtSignal, QThread, QTimer
from PyQt6.QtGui import QFont, QTextCursor, QPalette, QColor
from functools import partial

class SerialMonitorThread(QThread):
    data_received = pyqtSignal(str)

    def __init__(self, serial_port):
        super().__init__()
        self.serial_port = serial_port
        self.running = True

    def run(self):
        while self.running and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    data = self.serial_port.readline().decode().strip()
                    if data:
                        self.data_received.emit(data)
            except Exception as e:
                self.data_received.emit(f"Error reading serial: {str(e)}")
                # Disconnect on serial read error
                if hasattr(self.parent(), "disconnect"):
                    self.parent().disconnect()
                break
            self.msleep(10)

    def stop(self):
        self.running = False

class ESP32ControlGUI(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Ghost ESP Control Panel")
        self.setGeometry(100, 100, 1400, 900)

        # Initialize serial communication variables
        self.serial_port = None
        self.monitor_thread = None

        # Set dark theme
        self.setup_dark_theme()

        # Create central widget
        central_widget = QWidget()
        self.setCentralWidget(central_widget)

        # Create main layout for central widget
        main_layout = QVBoxLayout(central_widget)

        # Set up the main layout
        self.setup_ui(main_layout)

        # Refresh available ports
        self.refresh_ports()

        # --- Auto Reconnect Timer ---
        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.setInterval(2000)  # Check every 2 seconds
        self.reconnect_timer.timeout.connect(self.check_auto_reconnect)
        self.reconnect_timer.start()
        # --- End Auto Reconnect Timer ---

    def setup_dark_theme(self):
        palette = QPalette()
        palette.setColor(QPalette.ColorRole.Window, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.WindowText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Base, QColor(25, 25, 25))
        palette.setColor(QPalette.ColorRole.AlternateBase, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.ToolTipBase, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.ToolTipText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Text, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.Button, QColor(53, 53, 53))
        palette.setColor(QPalette.ColorRole.ButtonText, Qt.GlobalColor.white)
        palette.setColor(QPalette.ColorRole.BrightText, Qt.GlobalColor.red)
        palette.setColor(QPalette.ColorRole.Link, QColor(42, 130, 218))
        palette.setColor(QPalette.ColorRole.Highlight, QColor(42, 130, 218))
        palette.setColor(QPalette.ColorRole.HighlightedText, Qt.GlobalColor.black)
        self.setPalette(palette)

    def setup_ui(self, main_layout):
        # Create top bar for serial connection
        self.setup_connection_bar(main_layout)

        # Create splitter for resizable sections
        splitter = QSplitter(Qt.Orientation.Vertical)
        main_layout.addWidget(splitter)

        # Create main content area
        content_widget = QWidget()
        content_layout = QHBoxLayout(content_widget)
        splitter.addWidget(content_widget)

        # Left side - Command panels
        left_widget = QWidget()
        left_layout = QVBoxLayout(left_widget)
        self.setup_command_tabs(left_layout)
        content_layout.addWidget(left_widget)

        # Right side - Display area
        right_widget = QWidget()
        right_layout = QVBoxLayout(right_widget)
        self.setup_display_area(right_layout)
        content_layout.addWidget(right_widget)

        # Set content layout stretch factors
        content_layout.setStretch(0, 1)  # Left side
        content_layout.setStretch(1, 1)  # Right side

        # Bottom - Log area
        self.setup_log_area()
        splitter.addWidget(self.log_group)

        # Set splitter stretch factors
        splitter.setStretchFactor(0, 3)  # Content area
        splitter.setStretchFactor(1, 1)  # Log area

    def setup_connection_bar(self, main_layout):
        connection_group = QGroupBox("Serial Connection")
        connection_layout = QHBoxLayout(connection_group)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(150)
        connection_layout.addWidget(QLabel("Port:"))
        connection_layout.addWidget(self.port_combo)

        # --- Auto Reconnect Checkbox ---
        self.auto_reconnect_checkbox = QCheckBox("Auto Reconnect")
        self.auto_reconnect_checkbox.setChecked(False)
        connection_layout.addWidget(self.auto_reconnect_checkbox)
        # --- End Auto Reconnect Checkbox ---

        refresh_btn = QPushButton("Refresh Ports")
        refresh_btn.clicked.connect(self.refresh_ports)
        refresh_btn.setFixedWidth(100)
        connection_layout.addWidget(refresh_btn)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connection)
        self.connect_btn.setFixedWidth(100)
        connection_layout.addWidget(self.connect_btn)

        connection_layout.addStretch()
        main_layout.addWidget(connection_group)

    def setup_command_panels(self, layout):
        # Dropdown for panel selection
        self.panel_combo = QComboBox()
        self.panel_combo.addItems([
            "WiFi Operations",
            "Network Operations",
            "BLE Operations",
            "Capture Operations",
            "Evil Portal",
            "Settings"
        ])
        layout.addWidget(self.panel_combo)

        # Create each panel widget
        self.panels = []
        self.panels.append(self.create_wifi_tab())
        self.panels.append(self.create_network_tab())
        self.panels.append(self.create_ble_tab())
        self.panels.append(self.create_capture_tab())
        self.panels.append(self.create_evil_portal_tab())
        self.panels.append(self.create_settings_tab())

        # Container for panels
        self.panel_container = QWidget()
        self.panel_layout = QVBoxLayout(self.panel_container)
        for panel in self.panels:
            self.panel_layout.addWidget(panel)
            panel.hide()
        layout.addWidget(self.panel_container)

        # Show the first panel by default
        self.panels[0].show()

        # Connect dropdown change to panel switch
        self.panel_combo.currentIndexChanged.connect(self.switch_panel)

    def switch_panel(self, index):
        for i, panel in enumerate(self.panels):
            panel.setVisible(i == index)

    def setup_command_tabs(self, layout):
        self.setup_command_panels(layout)

    def create_wifi_tab(self):
        wifi_widget = QWidget()
        wifi_layout = QGridLayout(wifi_widget)

        # Scanning Operations
        self.create_command_group("WiFi Scanning", [
            ("Scan Access Points", "scanap"),
            ("Scan Stations", "scansta"),
            ("Stop Scan", "stopscan"),
            ("List APs", "list -a"),
            ("List Stations", "list -s")
        ], wifi_layout, 0, 0)

        # Attack Operations
        self.create_command_group("Attack Operations", [
            ("Start Deauth", "attack -d"),
            ("Stop Deauth", "stopdeauth"),
            ("Select AP", self.show_select_ap_dialog)
        ], wifi_layout, 0, 1)

        # Beacon Operations
        self.create_command_group("Beacon Operations", [
            ("Random Beacon Spam", "beaconspam -r"),
            ("Rickroll Beacon", "beaconspam -rr"),
            ("AP List Beacon", "beaconspam -l"),
            ("Custom SSID Beacon", self.show_custom_beacon_dialog),
            ("Stop Spam", "stopspam")
        ], wifi_layout, 1, 0)

        # Beacon List Management
        self.create_command_group("Beacon List Management", [
            ("Add SSID to List", self.show_beacon_add_dialog),
            ("Remove SSID from List", self.show_beacon_remove_dialog),
            ("Clear Beacon List", "beaconclear"),
            ("Show Beacon List", "beaconshow"),
            ("Spam Beacon List", "beaconspamlist"),
        ], wifi_layout, 1, 1)

        # Probe Request Listener
        probe_group = QGroupBox("Probe Request Listener")
        probe_layout = QHBoxLayout(probe_group)
        self.probe_channel = QLineEdit()
        self.probe_channel.setPlaceholderText("Channel (optional)")
        probe_layout.addWidget(self.probe_channel)
        start_probe_btn = QPushButton("Start Listening")
        start_probe_btn.clicked.connect(self.start_probe_listener)
        probe_layout.addWidget(start_probe_btn)
        stop_probe_btn = QPushButton("Stop Listening")
        stop_probe_btn.clicked.connect(lambda: self.send_command("listenprobes stop"))
        probe_layout.addWidget(stop_probe_btn)
        wifi_layout.addWidget(probe_group, 2, 0)

        return wifi_widget

    def create_network_tab(self):
        network_widget = QWidget()
        network_layout = QGridLayout(network_widget)

        # WiFi Connection
        wifi_connect_group = QGroupBox("WiFi Connection")
        wifi_connect_layout = QFormLayout(wifi_connect_group)

        self.wifi_ssid = QLineEdit()
        self.wifi_password = QLineEdit()
        self.wifi_password.setEchoMode(QLineEdit.EchoMode.Password)

        wifi_connect_layout.addRow("SSID:", self.wifi_ssid)
        wifi_connect_layout.addRow("Password:", self.wifi_password)

        connect_btn = QPushButton("Connect to Network")
        connect_btn.clicked.connect(self.connect_to_wifi)
        wifi_connect_layout.addRow(connect_btn)

        network_layout.addWidget(wifi_connect_group, 0, 0)

        # Network Tools
        self.create_command_group("Network Tools", [
            ("Cast Random YouTube Video", "dialconnect"),
            ("Print to Network Printer", self.show_printer_dialog)
        ], network_layout, 0, 1)

        # Port Scanner
        portscan_group = QGroupBox("Port Scanner")
        portscan_layout = QFormLayout(portscan_group)

        self.portscan_ip = QLineEdit()
        self.portscan_args = QLineEdit()
        portscan_layout.addRow("Target IP (or 'local'):", self.portscan_ip)
        portscan_layout.addRow("Args (-C, -A, or range):", self.portscan_args)

        scan_btn = QPushButton("Scan Ports")
        scan_btn.clicked.connect(self.run_port_scan)
        portscan_layout.addRow(scan_btn)

        network_layout.addWidget(portscan_group, 1, 0)

        return network_widget

    def create_ble_tab(self):
        ble_widget = QWidget()
        ble_layout = QGridLayout(ble_widget)

        self.create_command_group("BLE Scanning", [
            ("Find Flippers", "blescan -f"),
            ("BLE Spam Detector", "blescan -ds"),
            ("AirTag Scanner", "blescan -a"),
            ("Raw BLE Scan", "blescan -r"),
            ("Stop BLE Scan", "blescan -s")
        ], ble_layout, 0, 0)

        return ble_widget

    def create_capture_tab(self):
        capture_widget = QWidget()
        capture_layout = QGridLayout(capture_widget)

        self.create_command_group("Packet Capture (Requires SD Card or Flipper)", [
            ("Capture Probes", "capture -probe"),
            ("Capture Beacons", "capture -beacon"),
            ("Capture Deauth", "capture -deauth"),
            ("Capture Raw", "capture -raw"),
            ("Capture WPS", "capture -wps"),
            ("Capture Pwnagotchi", "capture -pwn"),
            ("Stop Capture", "capture -stop")
        ], capture_layout, 0, 0)

        return capture_widget

    def create_evil_portal_tab(self):
        portal_widget = QWidget()
        portal_layout = QFormLayout(portal_widget)

        # Portal Settings with default values
        self.portal_ssid = QLineEdit("FreeWiFi")
        self.portal_password = QLineEdit("password123")
        portal_layout.addRow("Portal SSID:", self.portal_ssid)
        portal_layout.addRow("Portal Password:", self.portal_password)

        # --- Available Portals Dropdown + Refresh Button ---
        dropdown_layout = QHBoxLayout()
        self.portal_dropdown = QComboBox()
        self.portal_dropdown.addItem("default")
        dropdown_layout.addWidget(self.portal_dropdown)

        list_portals_btn = QPushButton()
        list_portals_btn.setToolTip("Refresh Portal List")
        list_portals_btn.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_BrowserReload))
        list_portals_btn.setFixedSize(28, 28)
        list_portals_btn.clicked.connect(lambda: self.send_command("listportals"))
        dropdown_layout.addWidget(list_portals_btn)

        portal_layout.addRow("Available Portals:", dropdown_layout)

        # --- File selection button ---
        file_btn = QPushButton("Send Local HTML as Portal")
        file_btn.clicked.connect(self.send_local_portal_file)
        portal_layout.addRow(file_btn)
        # --- End File selection button ---

        # Control buttons
        button_layout = QHBoxLayout()
        start_portal_btn = QPushButton("Start Portal")
        start_portal_btn.clicked.connect(self.start_evil_portal)
        stop_portal_btn = QPushButton("Stop Portal")
        stop_portal_btn.clicked.connect(lambda: self.send_command("stopportal"))
        button_layout.addWidget(start_portal_btn)
        button_layout.addWidget(stop_portal_btn)
        portal_layout.addRow(button_layout)

        return portal_widget

    def create_settings_tab(self):
        settings_widget = QWidget()
        settings_layout = QFormLayout(settings_widget)

        # Channel Switch Delay
        channel_delay = QComboBox()
        channel_delay.addItems(["0.5s", "1s", "2s", "3s", "4s"])
        channel_delay.currentIndexChanged.connect(lambda i: self.send_command(f"setsetting 2 {i+1}"))
        settings_layout.addRow("Channel Switch Delay:", channel_delay)

        # Channel Hopping
        channel_hopping = QComboBox()
        channel_hopping.addItems(["Disabled", "Enabled"])
        channel_hopping.currentIndexChanged.connect(lambda i: self.send_command(f"setsetting 3 {i+1}"))
        settings_layout.addRow("Channel Hopping:", channel_hopping)

        # Random BLE MAC
        ble_mac = QComboBox()
        ble_mac.addItems(["Disabled", "Enabled"])
        ble_mac.currentIndexChanged.connect(lambda i: self.send_command(f"setsetting 4 {i+1}"))
        settings_layout.addRow("Random BLE MAC:", ble_mac)

        # Access Point
        ap_enable_combo = QComboBox()
        ap_enable_combo.addItems(["Enable", "Disable"])
        ap_enable_combo.currentIndexChanged.connect(lambda i: self.send_command(f"apenable {'on' if i == 0 else 'off'}"))
        settings_layout.addRow("Access Point:", ap_enable_combo)

        # Reboot and Save buttons side by side
        button_layout = QHBoxLayout()
        reboot_btn = QPushButton("Reboot")
        reboot_btn.clicked.connect(lambda: self.send_command("reboot"))
        button_layout.addWidget(reboot_btn)

        save_btn = QPushButton("Save Settings")
        save_btn.clicked.connect(lambda: self.send_command("savesetting"))
        button_layout.addWidget(save_btn)

        # Add the button layout to settings layout
        settings_layout.addRow(button_layout)

        # SD Card Settings
        sd_group = QGroupBox("SD Card Settings")
        sd_layout = QVBoxLayout(sd_group)

        sd_config_btn = QPushButton("Show SD Config")
        sd_config_btn.clicked.connect(lambda: self.send_command("sd_config"))
        sd_layout.addWidget(sd_config_btn)

        sd_save_btn = QPushButton("Save SD Config")
        sd_save_btn.clicked.connect(lambda: self.send_command("sd_save_config"))
        sd_layout.addWidget(sd_save_btn)

        sd_pins_mmc_btn = QPushButton("Set SDMMC Pins")
        sd_pins_mmc_btn.clicked.connect(self.show_sdmmc_dialog)
        sd_layout.addWidget(sd_pins_mmc_btn)

        sd_pins_spi_btn = QPushButton("Set SPI Pins")
        sd_pins_spi_btn.clicked.connect(self.show_sdspi_dialog)
        sd_layout.addWidget(sd_pins_spi_btn)

        settings_layout.addRow(sd_group)

        chipinfo_btn = QPushButton("Show Chip Info")
        chipinfo_btn.clicked.connect(lambda: self.send_command("chipinfo"))
        settings_layout.addRow(chipinfo_btn)

        # Timezone setting
        self.tz_entry = QLineEdit()
        tz_btn = QPushButton("Set Timezone")
        tz_btn.clicked.connect(lambda: self.send_command(f"timezone {self.tz_entry.text().strip()}"))
        settings_layout.addRow("Timezone:", self.tz_entry)
        settings_layout.addRow(tz_btn)

        rgbpins_btn = QPushButton("Set RGB Pins")
        rgbpins_btn.clicked.connect(self.show_rgbpins_dialog)
        settings_layout.addRow(rgbpins_btn)

        setrgbmode_combo = QComboBox()
        setrgbmode_combo.addItems(["Normal", "Rainbow", "Stealth"])
        setrgbmode_combo.currentIndexChanged.connect(lambda i: self.send_command(f"setrgbmode {setrgbmode_combo.currentText().lower()}"))
        settings_layout.addRow("Set RGB Mode:", setrgbmode_combo)

        return settings_widget

    def create_command_group(self, title, commands, layout, row, col):
        group = QGroupBox(title)
        group_layout = QVBoxLayout(group)

        for text, command in commands:
            btn = QPushButton(text)
            if callable(command):
                btn.clicked.connect(command)
            else:
                btn.clicked.connect(partial(self.send_command, command))
            group_layout.addWidget(btn)

        group_layout.addStretch()
        layout.addWidget(group, row, col)

    def setup_display_area(self, layout):
        display_group = QGroupBox("Display")
        display_layout = QVBoxLayout(display_group)

        self.display_text = QTextEdit()
        self.display_text.setReadOnly(True)
        display_layout.addWidget(self.display_text)

        button_layout = QHBoxLayout()
        clear_display_btn = QPushButton("Clear Display")
        clear_display_btn.clicked.connect(self.display_text.clear)
        button_layout.addWidget(clear_display_btn)

        save_log_btn = QPushButton("Save Log")
        save_log_btn.clicked.connect(self.save_log)
        button_layout.addWidget(save_log_btn)
        display_layout.addLayout(button_layout)

        # --- Custom Command Box ---
        cmd_layout = QHBoxLayout()
        self.cmd_entry = QLineEdit()
        self.cmd_entry.setPlaceholderText("Enter custom command...")
        self.cmd_entry.returnPressed.connect(self.send_custom_command)
        cmd_layout.addWidget(self.cmd_entry)
        send_cmd_btn = QPushButton("Send")
        send_cmd_btn.clicked.connect(self.send_custom_command)
        cmd_layout.addWidget(send_cmd_btn)
        display_layout.addLayout(cmd_layout)
        # --- End Custom Command Box ---

        layout.addWidget(display_group)

    def setup_log_area(self):
        self.log_group = QGroupBox("Log")
        log_layout = QVBoxLayout(self.log_group)

        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(200)
        log_layout.addWidget(self.log_text)

        button_layout = QHBoxLayout()
        clear_log_btn = QPushButton("Clear Log")
        clear_log_btn.clicked.connect(self.log_text.clear)
        button_layout.addWidget(clear_log_btn)

        save_log_btn = QPushButton("Save Log")
        save_log_btn.clicked.connect(self.save_log)
        button_layout.addWidget(save_log_btn)

        log_layout.addLayout(button_layout)

    def refresh_ports(self):
        self.port_combo.clear()
        ports = []
        for port in list_ports.comports():
            # Filter out system ports commonly found on Linux
            if port.device.startswith("/dev/ttyS"):
                continue
            if port.device.startswith("/dev/ttyAMA"):
                continue
            if port.device.startswith("/dev/ttyprintk"):
                continue
            if port.device.startswith("/dev/pts"):
                continue
            ports.append(port.device)
        self.port_combo.addItems(ports)

    def toggle_connection(self):
        if not self.serial_port or not self.serial_port.is_open:
            try:
                port = self.port_combo.currentText()
                self.serial_port = serial.Serial(port, 115200, timeout=1)
                self.connect_btn.setText("Disconnect")
                self.connect_btn.setStyleSheet("background-color: #ff4444;")
                self.log_message(f"Connected to {port}")

                # Start monitor thread
                self.monitor_thread = SerialMonitorThread(self.serial_port)
                self.monitor_thread.data_received.connect(self.process_response)
                self.monitor_thread.start()

                # Enable auto-reconnect only after manual connect
                self.auto_reconnect_enabled = True

            except Exception as e:
                QMessageBox.critical(self, "Connection Error", str(e))
                self.log_message(f"Connection error: {str(e)}")
        else:
            self.disconnect()
            self.auto_reconnect_enabled = False  # Disable auto-reconnect on manual disconnect

    def disconnect(self):
        if self.monitor_thread:
            self.monitor_thread.stop()
            self.monitor_thread.wait()

        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()

        self.connect_btn.setText("Connect")
        self.connect_btn.setStyleSheet("")
        self.log_message("Disconnected")

    def send_command(self, command):
        if not self.serial_port or not self.serial_port.is_open:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        self.log_message(f"Sending command: {command}")
        try:
            self.serial_port.write(f"{command}\n".encode())
        except Exception as e:
            self.log_message(f"Error sending command: {str(e)}")
            self.disconnect()  # Disconnect

    def send_custom_command(self):
        command = self.cmd_entry.text().strip()
        if command:
            self.send_command(command)
            self.cmd_entry.clear()

    def process_response(self, response):
        if response.startswith("Error reading serial:"):
            self.log_message(response)
            self.disconnect()
            return

        # Check for evil portal list output
        if "Available Evil Portals:" in response or (
            hasattr(self, "_portal_list_mode") and self._portal_list_mode
        ):
            # Start portal list mode if header detected
            if "Available Evil Portals:" in response:
                self.portal_dropdown.clear()
                self.portal_dropdown.addItem("default")  # Always add "default" portal
                self._portal_list_mode = True
                self._portal_lines = []
                # Don't return yet, continue to parse this line

            # Parse lines for .html files, stripping timestamps
            lines = response.splitlines()
            for line in lines:
                line = line.strip()
                # Remove timestamp if present
                if "] " in line:
                    line = line.split("] ", 1)[-1]
                if line.endswith(".html"):
                    self._portal_lines.append(line)
                    self.portal_dropdown.addItem(line)
            self.display_text.append(response)
            self.display_text.ensureCursorVisible()
            return

        try:
            # Try to parse as JSON for structured data
            data = json.loads(response)
            if 'scan_result' in data:
                self.update_display_scan(data['scan_result'])
            elif 'status' in data:
                self.update_display_status(data['status'])
            else:
                self.display_text.append(response)

        except json.JSONDecodeError:
            # Format the text with timestamp
            timestamp = datetime.now().strftime("%H:%M:%S")
            formatted_text = f"[{timestamp}] {response}"
            self.display_text.append(formatted_text)

        self.display_text.ensureCursorVisible()

    def log_message(self, message):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self.log_text.append(f"[{timestamp}] {message}")
        self.log_text.ensureCursorVisible()

    def save_log(self):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"ghost_esp_log_{timestamp}.txt"
        try:
            with open(filename, 'w') as f:
                f.write(self.display_text.toPlainText())
            self.log_message(f"Log saved to {filename}")
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to save log: {str(e)}")

    def show_select_ap_dialog(self):
        selected_ap, ok = QInputDialog.getText(self, "Select Access Point", "Enter Access Point name:")
        if ok and selected_ap:
            self.send_command(f"select -a {selected_ap}")

    def show_custom_beacon_dialog(self):
        ssid, ok = QInputDialog.getText(self, "Custom Beacon", "Enter SSID for beacon spam:")
        if ok and ssid:
            self.send_command(f'beaconspam "{ssid}"')

    def show_printer_dialog(self):
        dialog = QDialog(self)
        dialog.setWindowTitle("Print to Network Printer")
        layout = QFormLayout(dialog)

        ip_input = QLineEdit()
        text_input = QTextEdit()
        font_size = QSpinBox()
        font_size.setRange(8, 72)
        font_size.setValue(12)

        alignment = QComboBox()
        alignment.addItems(["Center Middle (CM)", "Top Left (TL)", "Top Right (TR)",
                          "Bottom Right (BR)", "Bottom Left (BL)"])

        layout.addRow("Printer IP:", ip_input)
        layout.addRow("Text:", text_input)
        layout.addRow("Font Size:", font_size)
        layout.addRow("Alignment:", alignment)

        buttons = QHBoxLayout()
        ok_button = QPushButton("Print")
        cancel_button = QPushButton("Cancel")
        buttons.addWidget(ok_button)
        buttons.addWidget(cancel_button)
        layout.addRow(buttons)

        ok_button.clicked.connect(dialog.accept)
        cancel_button.clicked.connect(dialog.reject)

        if dialog.exec() == QDialog.DialogCode.Accepted:
            align_map = {"Center Middle (CM)": "CM", "Top Left (TL)": "TL",
                        "Top Right (TR)": "TR", "Bottom Right (BR)": "BR",
                        "Bottom Left (BL)": "BL"}
            cmd = f'powerprinter {ip_input.text()} "{text_input.toPlainText()}" {font_size.value()} {align_map[alignment.currentText()]}'
            self.send_command(cmd)

    def connect_to_wifi(self):
        ssid = self.wifi_ssid.text()
        password = self.wifi_password.text()
        if ssid and password:
            self.send_command(f"connect {ssid} {password}")
        else:
            QMessageBox.warning(self, "Input Error", "Please enter both SSID and password")

    def start_evil_portal(self):
        ssid = self.portal_ssid.text()
        password = self.portal_password.text()
        portal_file = self.portal_dropdown.currentText()

        if all([ssid, password, portal_file]):
            cmd = f"startportal {portal_file} {ssid} {password}"
            self.send_command(cmd)
        else:
            QMessageBox.warning(self, "Input Error", "Please fill all required fields and select a portal")

    def run_port_scan(self):
        ip = self.portscan_ip.text().strip()
        args = self.portscan_args.text().strip()
        if ip and args:
            self.send_command(f"scanports {ip} {args}")
        else:
            QMessageBox.warning(self, "Input Error", "Please enter both IP and arguments")

    def update_display_scan(self, scan_data):
        self.display_text.append("\n=== Scan Results ===")
        for item in scan_data:
            self.display_text.append(f"- {item}")
        self.display_text.append("==================\n")
        self.display_text.ensureCursorVisible()

    def update_display_status(self, status):
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.display_text.append(f"[{timestamp}] Status: {status}")
        self.display_text.ensureCursorVisible()

    def closeEvent(self, event):
        if self.serial_port and self.serial_port.is_open:
            self.disconnect()
        super().closeEvent(event)

    def show_beacon_add_dialog(self):
        ssid, ok = QInputDialog.getText(self, "Add SSID", "Enter SSID to add to beacon list:")
        if ok and ssid:
            self.send_command(f'beaconadd "{ssid}"')

    def show_beacon_remove_dialog(self):
        ssid, ok = QInputDialog.getText(self, "Remove SSID", "Enter SSID to remove from beacon list:")
        if ok and ssid:
            self.send_command(f'beaconremove "{ssid}"')

    def show_sdmmc_dialog(self):
        pins, ok = QInputDialog.getText(self, "Set SDMMC Pins", "Enter pins: clk cmd d0 d1 d2 d3 (space-separated)")
        if ok and pins:
            self.send_command(f"sd_pins_mmc {pins}")

    def show_sdspi_dialog(self):
        pins, ok = QInputDialog.getText(self, "Set SPI Pins", "Enter pins: cs clk miso mosi (space-separated)")
        if ok and pins:
            self.send_command(f"sd_pins_spi {pins}")

    def start_probe_listener(self):
        channel = self.probe_channel.text().strip()
        if channel:
            self.send_command(f"listenprobes {channel}")
        else:
            self.send_command("listenprobes")

    def show_rgbpins_dialog(self):
        pins, ok = QInputDialog.getText(self, "Set RGB Pins", "Enter RGB pins (R1 G1 B1 R2 G2 B2 ...):")
        if ok and pins:
            self.send_command(f"rgb_pins {pins}")

    def send_local_portal_file(self):
        file_path, _ = QFileDialog.getOpenFileName(self, "Select HTML File", "", "HTML Files (*.html *.htm)")
        if file_path:
            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    html_content = f.read()
                ssid = self.portal_ssid.text().strip()
                password = self.portal_password.text().strip()
                safe_html = html_content.replace('"', '\\"')  # Only escape quotes, keep lines

                cmd = f'evilportal -c sethtmlstr'
                self.send_command(cmd)
                time.sleep(0.2)
                self.send_command('[HTML/BEGIN]')
                time.sleep(0.2)

                # Send each line as a separate command
                chunk_size = 256
                for i in range(0, len(safe_html), chunk_size):
                    self.send_command(safe_html[i:i+chunk_size])
                    time.sleep(0.05)
                time.sleep(0.2)
                self.send_command('[HTML/CLOSE]')
                QMessageBox.information(self, "Portal Sent", "HTML file sent as evil portal.")
            except Exception as e:
                QMessageBox.critical(self, "Error", f"Failed to send file: {str(e)}")

    def check_auto_reconnect(self):
        # Only auto-reconnect if enabled and checkbox is checked
        if getattr(self, "auto_reconnect_enabled", False) and self.auto_reconnect_checkbox.isChecked():
            if not self.serial_port or not self.serial_port.is_open:
                port = self.port_combo.currentText()
                if port:
                    try:
                        self.serial_port = serial.Serial(port, 115200, timeout=1)
                        self.connect_btn.setText("Disconnect")
                        self.connect_btn.setStyleSheet("background-color: #ff4444;")
                        self.log_message(f"Auto-reconnected to {port}")

                        self.monitor_thread = SerialMonitorThread(self.serial_port)
                        self.monitor_thread.data_received.connect(self.process_response)
                        self.monitor_thread.start()
                    except Exception as e:
                        self.log_message(f"Auto-reconnect failed: {str(e)}")

        self.auto_reconnect_checkbox.stateChanged.connect(self.toggle_reconnect_timer)

    def toggle_reconnect_timer(self, state):
        if state:
            self.reconnect_timer.start()
        else:
            self.reconnect_timer.stop()
if __name__ == "__main__":
    app = QApplication(sys.argv)
    font = QFont("Arial", 10)
    app.setFont(font)
    window = ESP32ControlGUI()
    window.show()
    sys.exit(app.exec())
