#!/usr/bin/env python3
"""
GhostESP Screen Mirror - Desktop Receiver
Receives display frames from ESP32 running GhostESP via USB serial.

Usage:
    python ghost_mirror.py [COM_PORT] [--scale N]
    
Example:
    python ghost_mirror.py COM3
    python ghost_mirror.py COM3 --scale 2
"""

import sys
import subprocess

def install_and_import(package, import_name=None):
    if import_name is None:
        import_name = package
    try:
        return __import__(import_name)
    except ImportError:
        print(f"Installing {package}...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", package])
        return __import__(import_name)

install_and_import("numpy")
install_and_import("pyserial", "serial")
install_and_import("pygame")

import struct
import time
import argparse
import threading
from collections import deque
import serial
import serial.tools.list_ports
import numpy as np
import pygame
import pygame.surfarray

MIRROR_MARKER = 0x47455350  # "GESP"
MIRROR_END_MARKER = 0x444E4547  # "GEND"
MIRROR_CMD_INFO = 0x01
MIRROR_CMD_FRAME = 0x02

HEADER_SIZE = 17  # 4 + 1 + 2 + 2 + 2 + 2 + 4

class Theme:
    BG_DARK = (18, 18, 22)
    BG_PANEL = (28, 28, 32)
    BG_HEADER = (35, 35, 42)
    ACCENT = (240, 240, 240)
    ACCENT_DIM = (180, 180, 180)
    TEXT = (220, 220, 225)
    TEXT_DIM = (120, 120, 130)
    SUCCESS = (34, 197, 94)
    WARNING = (250, 204, 21)
    ERROR = (239, 68, 68)
    BORDER = (55, 55, 65)

class GhostMirror:
    TITLE_BAR_HEIGHT = 32
    HEADER_HEIGHT = 36
    STATUS_HEIGHT = 28
    PADDING = 12
    CONTROLS_WIDTH = 100
    
    def __init__(self, port, baudrate=115200, scale=1):
        self.port = port
        self.baudrate = baudrate
        self.scale = scale
        self.swap_bytes = False
        self.serial = None
        self.running = False
        self.width = 320
        self.height = 240
        self.bpp = 16
        self.screen = None
        self.surface = None
        self.pixel_array = None
        self.frame_queue = deque(maxlen=30)
        self.fps_counter = 0
        self.fps_time = time.time()
        self.fps = 0
        self.frame_count = 0
        self.resize_pending = False
        self.new_width = 320
        self.new_height = 240
        self.connected = False
        self.status_msg = "Connecting..."
        self.font = None
        self.font_small = None
        self.font_bold = None
        self.buttons = []
        self.hovered_btn = None
        self.last_data_time = 0
        self.close_btn = None
        self.connect_btn = None
        self.disconnect_btn = None
        self.swap_btn = None
        self.port_prev_btn = None
        self.port_next_btn = None
        self.available_ports = []
        self.port_index = 0
        self.scale_down_btn = None
        self.scale_up_btn = None
        
    def connect(self):
        try:
            if self.serial:
                try:
                    self.serial.close()
                except:
                    pass
            self.serial = serial.Serial()
            self.serial.port = self.port
            self.serial.baudrate = self.baudrate
            self.serial.timeout = 0.1
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.open()
            self.status_msg = f"Connected to {self.port}"
            self.connected = True
            self.last_data_time = time.time()
            time.sleep(0.3)
            self.serial.write(b"mirror on\n")
            return True
        except Exception as e:
            self.status_msg = f"Connection failed"
            self.connected = False
            return False
    
    def disconnect(self):
        if self.serial:
            try:
                self.serial.write(b"mirror off\n")
            except:
                pass
            try:
                self.serial.close()
            except:
                pass
        self.serial = None
        self.connected = False
    
    def refresh_ports(self):
        self.available_ports = [p.device for p in serial.tools.list_ports.comports()]
        if self.port in self.available_ports:
            self.port_index = self.available_ports.index(self.port)
        elif self.available_ports:
            self.port_index = 0
            self.port = self.available_ports[0]
    
    def select_port(self, direction):
        if not self.available_ports:
            self.refresh_ports()
        if self.available_ports:
            self.port_index = (self.port_index + direction) % len(self.available_ports)
            self.port = self.available_ports[self.port_index]
    
    def change_scale(self, delta):
        new_scale = max(1, min(4, self.scale + delta))
        if new_scale != self.scale:
            self.scale = new_scale
            self.resize_pending = True
            self.new_width = self.width
            self.new_height = self.height
            self.pixel_array.fill(0)
            if self.serial and self.connected:
                try:
                    self.serial.write(b"mirror refresh\n")
                except:
                    pass
    
    def get_window_size(self):
        content_w = self.width * self.scale
        content_h = self.height * self.scale
        total_w = content_w + self.PADDING * 3 + self.CONTROLS_WIDTH
        total_h = content_h + self.TITLE_BAR_HEIGHT + self.HEADER_HEIGHT + self.STATUS_HEIGHT + self.PADDING * 2
        return total_w, total_h
    
    def send_input(self, direction):
        if self.serial and self.connected:
            cmd = f"input {direction}\n".encode()
            try:
                self.serial.write(cmd)
            except:
                pass
    
    def setup_buttons(self):
        content_w = self.width * self.scale
        btn_size = 28
        gap = 8
        dpad_width = btn_size * 3 + gap * 2
        ctrl_x = self.PADDING * 2 + content_w + (self.CONTROLS_WIDTH - dpad_width) // 2
        ctrl_y = self.TITLE_BAR_HEIGHT + self.HEADER_HEIGHT + self.PADDING + 20
        center_x = ctrl_x + btn_size + gap
        center_y = ctrl_y + btn_size + gap
        left_x = ctrl_x
        right_x = ctrl_x + 2 * (btn_size + gap)
        top_y = ctrl_y
        bottom_y = ctrl_y + 2 * (btn_size + gap)

        self.buttons = [
            {'rect': pygame.Rect(center_x, top_y, btn_size, btn_size), 'label': '▲', 'cmd': 'up'},
            {'rect': pygame.Rect(left_x, center_y, btn_size, btn_size), 'label': '◄', 'cmd': 'left'},
            {'rect': pygame.Rect(center_x, center_y, btn_size, btn_size), 'label': '●', 'cmd': 'select'},
            {'rect': pygame.Rect(right_x, center_y, btn_size, btn_size), 'label': '►', 'cmd': 'right'},
            {'rect': pygame.Rect(center_x, bottom_y, btn_size, btn_size), 'label': '▼', 'cmd': 'down'},
        ]
        
        action_y = ctrl_y + 145
        action_w = 80
        action_h = 22
        action_x = self.PADDING * 2 + content_w + (self.CONTROLS_WIDTH - action_w) // 2
        self.connect_btn = pygame.Rect(action_x, action_y, action_w, action_h)
        self.disconnect_btn = pygame.Rect(action_x, action_y + 28, action_w, action_h)
        self.swap_btn = pygame.Rect(action_x, action_y + 56, action_w, action_h)
    
    def init_display(self):
        pygame.init()
        pygame.display.set_caption("GhostESP Mirror")
        
        self.font = pygame.font.SysFont("Segoe UI", 13)
        self.font_small = pygame.font.SysFont("Segoe UI", 11)
        self.font_bold = pygame.font.SysFont("Segoe UI", 14, bold=True)
        
        win_w, win_h = self.get_window_size()
        self.screen = pygame.display.set_mode((win_w, win_h), pygame.NOFRAME)
        self.surface = pygame.Surface((self.width, self.height))
        self.surface.fill((0, 0, 0))
        self.pixel_array = np.zeros((self.width, self.height, 3), dtype=np.uint8)
        self.setup_buttons()
        self.setup_title_bar_buttons()
    
    def setup_title_bar_buttons(self):
        win_w, _ = self.get_window_size()
        btn_size = 20
        btn_y = (self.TITLE_BAR_HEIGHT - btn_size) // 2
        self.close_btn = pygame.Rect(win_w - btn_size - 8, btn_y, btn_size, btn_size)
        
    def resize_display(self):
        self.width = self.new_width
        self.height = self.new_height
        win_w, win_h = self.get_window_size()
        self.screen = pygame.display.set_mode((win_w, win_h), pygame.NOFRAME)
        self.surface = pygame.Surface((self.width, self.height))
        self.surface.fill((0, 0, 0))
        self.pixel_array = np.zeros((self.width, self.height, 3), dtype=np.uint8)
        self.resize_pending = False
        self.status_msg = f"Display: {self.width}x{self.height}"
        self.setup_buttons()
        self.setup_title_bar_buttons()
    
    def find_marker(self, data):
        for i in range(len(data) - 3):
            val = struct.unpack('<I', data[i:i+4])[0]
            if val == MIRROR_MARKER:
                return i
        return -1
    
    def read_thread(self):
        buffer = bytearray()
        
        while self.running:
            try:
                if not self.serial or not self.serial.is_open:
                    time.sleep(0.5)
                    continue
                chunk = self.serial.read(4096)
                if chunk:
                    buffer.extend(chunk)
                    self.last_data_time = time.time()
                    if not self.connected:
                        self.connected = True
                        self.status_msg = f"Connected to {self.port}"
                    
                while len(buffer) >= HEADER_SIZE:
                    marker_pos = self.find_marker(buffer)
                    if marker_pos < 0:
                        buffer = buffer[-4:] if len(buffer) > 4 else buffer
                        break
                    
                    if marker_pos > 0:
                        buffer = buffer[marker_pos:]
                    
                    if len(buffer) < HEADER_SIZE:
                        break
                        
                    marker, cmd, x1, y1, x2, y2, data_len = struct.unpack(
                        '<IBHHHHI', buffer[:HEADER_SIZE]
                    )
                    
                    if cmd == MIRROR_CMD_INFO:
                        if x1 != self.width or y1 != self.height:
                            self.new_width = x1
                            self.new_height = y1
                            self.resize_pending = True
                        self.bpp = x2
                        print(f"Display info: {x1}x{y1} @ {self.bpp}bpp")
                        buffer = buffer[HEADER_SIZE:]
                        continue
                        
                    if cmd == MIRROR_CMD_FRAME or cmd == MIRROR_CMD_FRAME_RLE or cmd == MIRROR_CMD_FRAME_8BIT or cmd == MIRROR_CMD_FRAME_8BIT_RLE:
                        total_needed = HEADER_SIZE + data_len + 4  # +4 for end marker
                        if len(buffer) < total_needed:
                            break
                        
                        pixel_data = buffer[HEADER_SIZE:HEADER_SIZE + data_len]
                        end_marker = struct.unpack('<I', buffer[HEADER_SIZE + data_len:total_needed])[0]
                        buffer = buffer[total_needed:]
                        
                        if end_marker == MIRROR_END_MARKER and data_len > 0:
                            is_8bit = (cmd == MIRROR_CMD_FRAME_8BIT or cmd == MIRROR_CMD_FRAME_8BIT_RLE)
                            is_rle = (cmd == MIRROR_CMD_FRAME_RLE or cmd == MIRROR_CMD_FRAME_8BIT_RLE)
                            self.frame_queue.append((x1, y1, x2, y2, pixel_data, is_8bit, is_rle))
                    else:
                        buffer = buffer[1:]
                        
            except Exception as e:
                if self.running:
                    self.connected = False
                    time.sleep(0.5)
    
    def process_frame(self, x1, y1, x2, y2, pixel_data, is_8bit=False, is_rle=False):
        w = x2 - x1 + 1
        h = y2 - y1 + 1
        
        if is_rle:
            # Simple RLE decode
            # data is [count, val, count, val, ...]
            # We need to decode it to raw pixels
            if is_8bit:
                # 8-bit RLE: count(1) val(1)
                data_arr = np.frombuffer(pixel_data, dtype=np.uint8)
                counts = data_arr[0::2]
                vals = data_arr[1::2]
                pixels_raw = np.repeat(vals, counts)
            else:
                # 16-bit RLE: count(1) val(2) - Not standard in this firmware but for completeness
                # Firmware implementation of RLE is specific to 8-bit currently in my analysis,
                # but if 16-bit RLE exists: count(1), val_lo(1), val_hi(1)? Or count(1) val(2)?
                # Assuming 8-bit RLE is the only one used by current firmware logic.
                pass
        else:
            # Raw data
            if is_8bit:
                pixels_raw = np.frombuffer(pixel_data, dtype=np.uint8)
            else:
                expected_size = w * h * 2
                if len(pixel_data) < expected_size:
                    return
                if self.swap_bytes:
                    pixels_raw = np.frombuffer(pixel_data[:expected_size], dtype='>u2')
                else:
                    pixels_raw = np.frombuffer(pixel_data[:expected_size], dtype=np.uint16)

        # Convert to RGB888
        if is_8bit:
            # RGB332 to RGB888 using linear scaling
            # R: bits 7-5 (0-7) -> 0-255
            # G: bits 4-2 (0-7) -> 0-255
            # B: bits 1-0 (0-3) -> 0-255

            r_bits = (pixels_raw >> 5) & 0x07
            g_bits = (pixels_raw >> 2) & 0x07
            b_bits = pixels_raw & 0x03

            r = (r_bits * 255) // 7
            g = (g_bits * 255) // 7
            b = (b_bits * 255) // 3

            rgb = np.stack([r, g, b], axis=-1).astype(np.uint8)
            # Ensure shape matches
            if len(rgb) == w * h:
                rgb = rgb.reshape(h, w, 3)
            else:
                 # fallback/error
                 return
        else:
            # RGB565 to RGB888
            r = ((pixels_raw >> 11) & 0x1F) << 3
            g = ((pixels_raw >> 5) & 0x3F) << 2
            b = (pixels_raw & 0x1F) << 3
            
            # Fill lower bits
            r = r | (r >> 5)
            g = g | (g >> 6)
            b = b | (b >> 5)
            
            rgb = np.stack([r, g, b], axis=-1).astype(np.uint8).reshape(h, w, 3)
        
        x_end = min(x1 + w, self.width)
        y_end = min(y1 + h, self.height)
        actual_w = x_end - x1
        actual_h = y_end - y1
        
        if actual_w > 0 and actual_h > 0 and x1 < self.width and y1 < self.height:
            self.pixel_array[x1:x_end, y1:y_end] = rgb[:actual_h, :actual_w].transpose(1, 0, 2)
        
        self.fps_counter += 1
        self.frame_count += 1
        now = time.time()
        if now - self.fps_time >= 1.0:
            self.fps = self.fps_counter
            self.fps_counter = 0
            self.fps_time = now
    
    def draw_rounded_rect(self, surface, color, rect, radius=6):
        x, y, w, h = rect
        pygame.draw.rect(surface, color, (x + radius, y, w - 2*radius, h))
        pygame.draw.rect(surface, color, (x, y + radius, w, h - 2*radius))
        pygame.draw.circle(surface, color, (x + radius, y + radius), radius)
        pygame.draw.circle(surface, color, (x + w - radius, y + radius), radius)
        pygame.draw.circle(surface, color, (x + radius, y + h - radius), radius)
        pygame.draw.circle(surface, color, (x + w - radius, y + h - radius), radius)
    
    def draw_gui(self):
        win_w, win_h = self.screen.get_size()
        content_w = self.width * self.scale
        content_h = self.height * self.scale
        mouse_pos = pygame.mouse.get_pos()
        
        self.screen.fill(Theme.BG_DARK)
        
        pygame.draw.rect(self.screen, (12, 12, 15), (0, 0, win_w, self.TITLE_BAR_HEIGHT))
        pygame.draw.line(self.screen, Theme.BORDER, (0, self.TITLE_BAR_HEIGHT - 1), (win_w, self.TITLE_BAR_HEIGHT - 1), 1)
        
        title = self.font_small.render("GhostESP Mirror", True, Theme.TEXT_DIM)
        self.screen.blit(title, (10, (self.TITLE_BAR_HEIGHT - title.get_height()) // 2))
        
        close_hover = self.close_btn and self.close_btn.collidepoint(mouse_pos)
        pygame.draw.rect(self.screen, Theme.ERROR if close_hover else Theme.BG_PANEL, self.close_btn, border_radius=3)
        close_x = self.font_small.render("×", True, Theme.TEXT)
        self.screen.blit(close_x, (self.close_btn.centerx - close_x.get_width() // 2, self.close_btn.centery - close_x.get_height() // 2 - 1))
        
        header_y = self.TITLE_BAR_HEIGHT
        pygame.draw.rect(self.screen, Theme.BG_HEADER, (0, header_y, win_w, self.HEADER_HEIGHT))
        pygame.draw.line(self.screen, Theme.ACCENT, (0, header_y + self.HEADER_HEIGHT - 1), (win_w, header_y + self.HEADER_HEIGHT - 1), 2)
        
        port_label = self.font_small.render("Port:", True, Theme.TEXT_DIM)
        self.screen.blit(port_label, (self.PADDING, header_y + (self.HEADER_HEIGHT - port_label.get_height()) // 2))
        
        btn_h = 20
        btn_y = header_y + (self.HEADER_HEIGHT - btn_h) // 2
        self.port_prev_btn = pygame.Rect(self.PADDING + port_label.get_width() + 8, btn_y, 20, btn_h)
        prev_hover = self.port_prev_btn.collidepoint(mouse_pos)
        pygame.draw.rect(self.screen, Theme.ACCENT if prev_hover else Theme.BG_PANEL, self.port_prev_btn, border_radius=3)
        prev_text = self.font_small.render("◄", True, Theme.TEXT)
        self.screen.blit(prev_text, (self.port_prev_btn.centerx - prev_text.get_width() // 2, self.port_prev_btn.centery - prev_text.get_height() // 2))
        
        port_text = self.font.render(self.port, True, Theme.TEXT)
        port_x = self.port_prev_btn.right + 8
        self.screen.blit(port_text, (port_x, header_y + (self.HEADER_HEIGHT - port_text.get_height()) // 2))
        
        self.port_next_btn = pygame.Rect(port_x + port_text.get_width() + 8, btn_y, 20, btn_h)
        next_hover = self.port_next_btn.collidepoint(mouse_pos)
        pygame.draw.rect(self.screen, Theme.ACCENT if next_hover else Theme.BG_PANEL, self.port_next_btn, border_radius=3)
        next_text = self.font_small.render("►", True, Theme.TEXT)
        self.screen.blit(next_text, (self.port_next_btn.centerx - next_text.get_width() // 2, self.port_next_btn.centery - next_text.get_height() // 2))
        
        status_color = Theme.SUCCESS if self.connected else Theme.ERROR
        status_dot_y = header_y + self.HEADER_HEIGHT // 2
        pygame.draw.circle(self.screen, status_color, (win_w - self.PADDING - 6, status_dot_y), 5)
        
        content_x = self.PADDING
        content_y = self.TITLE_BAR_HEIGHT + self.HEADER_HEIGHT + self.PADDING // 2
        
        self.draw_rounded_rect(self.screen, Theme.BG_PANEL, (content_x - 2, content_y - 2, content_w + 4, content_h + 4), 4)
        pygame.draw.rect(self.screen, Theme.BORDER, (content_x - 2, content_y - 2, content_w + 4, content_h + 4), 1, border_radius=4)
        
        pygame.surfarray.blit_array(self.surface, self.pixel_array)
        if self.scale == 1:
            self.screen.blit(self.surface, (content_x, content_y))
        else:
            scaled = pygame.transform.scale(self.surface, (content_w, content_h))
            self.screen.blit(scaled, (content_x, content_y))
        
        if not self.connected:
            overlay = pygame.Surface((content_w, content_h), pygame.SRCALPHA)
            overlay.fill((0, 0, 0, 180))
            self.screen.blit(overlay, (content_x, content_y))
            disc_text = self.font_bold.render("Disconnected", True, Theme.TEXT_DIM)
            self.screen.blit(disc_text, (content_x + (content_w - disc_text.get_width()) // 2, content_y + (content_h - disc_text.get_height()) // 2))
        
        ctrl_panel_x = content_x + content_w + self.PADDING
        ctrl_label = self.font_small.render("Controls", True, Theme.TEXT_DIM)
        self.screen.blit(ctrl_label, (ctrl_panel_x + (self.CONTROLS_WIDTH - ctrl_label.get_width()) // 2, content_y))
        
        for btn in self.buttons:
            is_hover = btn['rect'].collidepoint(mouse_pos)
            bg_color = Theme.ACCENT if is_hover else Theme.BG_PANEL
            pygame.draw.rect(self.screen, bg_color, btn['rect'], border_radius=4)
            pygame.draw.rect(self.screen, Theme.BORDER, btn['rect'], 1, border_radius=4)
            label = self.font_small.render(btn['label'], True, Theme.TEXT)
            label_x = btn['rect'].centerx - label.get_width() // 2
            label_y = btn['rect'].centery - label.get_height() // 2
            self.screen.blit(label, (label_x, label_y))
        
        keys_hint = self.font_small.render("WASD/Arrows", True, Theme.TEXT_DIM)
        hint_y = self.buttons[-1]['rect'].bottom + 8 if self.buttons else content_y + 130
        self.screen.blit(keys_hint, (ctrl_panel_x + (self.CONTROLS_WIDTH - keys_hint.get_width()) // 2, hint_y))
        
        separator_y = hint_y + keys_hint.get_height() + 10
        pygame.draw.line(self.screen, Theme.BORDER, (ctrl_panel_x + 10, separator_y), (ctrl_panel_x + self.CONTROLS_WIDTH - 10, separator_y), 1)
        
        if self.connect_btn:
            conn_hover = self.connect_btn.collidepoint(mouse_pos)
            conn_color = Theme.SUCCESS if conn_hover else Theme.BG_PANEL
            pygame.draw.rect(self.screen, conn_color, self.connect_btn, border_radius=3)
            pygame.draw.rect(self.screen, Theme.BORDER, self.connect_btn, 1, border_radius=3)
            conn_text = self.font_small.render("Connect", True, Theme.TEXT)
            self.screen.blit(conn_text, (self.connect_btn.centerx - conn_text.get_width() // 2, self.connect_btn.centery - conn_text.get_height() // 2))
        
        if self.disconnect_btn:
            disc_hover = self.disconnect_btn.collidepoint(mouse_pos)
            disc_color = Theme.ERROR if disc_hover else Theme.BG_PANEL
            pygame.draw.rect(self.screen, disc_color, self.disconnect_btn, border_radius=3)
            pygame.draw.rect(self.screen, Theme.BORDER, self.disconnect_btn, 1, border_radius=3)
            disc_text = self.font_small.render("Disconnect", True, Theme.TEXT)
            self.screen.blit(disc_text, (self.disconnect_btn.centerx - disc_text.get_width() // 2, self.disconnect_btn.centery - disc_text.get_height() // 2))
        
        if self.swap_btn:
            swap_hover = self.swap_btn.collidepoint(mouse_pos)
            swap_color = Theme.ACCENT if self.swap_bytes else (Theme.ACCENT_DIM if swap_hover else Theme.BG_PANEL)
            pygame.draw.rect(self.screen, swap_color, self.swap_btn, border_radius=3)
            pygame.draw.rect(self.screen, Theme.BORDER, self.swap_btn, 1, border_radius=3)
            swap_label = "Swap: ON" if self.swap_bytes else "Swap: OFF"
            swap_text_color = (0, 0, 0) if self.swap_bytes else Theme.TEXT
            swap_text = self.font_small.render(swap_label, True, swap_text_color)
            self.screen.blit(swap_text, (self.swap_btn.centerx - swap_text.get_width() // 2, self.swap_btn.centery - swap_text.get_height() // 2))
        
        status_y = win_h - self.STATUS_HEIGHT
        pygame.draw.rect(self.screen, Theme.BG_HEADER, (0, status_y, win_w, self.STATUS_HEIGHT))
        pygame.draw.line(self.screen, Theme.BORDER, (0, status_y), (win_w, status_y), 1)
        
        res_text = self.font_small.render(f"{self.width}×{self.height}", True, Theme.TEXT_DIM)
        self.screen.blit(res_text, (self.PADDING, status_y + (self.STATUS_HEIGHT - res_text.get_height()) // 2))
        
        fps_color = Theme.SUCCESS if self.fps >= 10 else Theme.WARNING if self.fps >= 5 else Theme.TEXT_DIM
        fps_text = self.font_small.render(f"{self.fps} FPS", True, fps_color)
        fps_x = self.PADDING + res_text.get_width() + 20
        self.screen.blit(fps_text, (fps_x, status_y + (self.STATUS_HEIGHT - fps_text.get_height()) // 2))
        
        scale_label = self.font_small.render(f"{self.scale}x", True, Theme.TEXT)
        scale_x = fps_x + fps_text.get_width() + 30
        btn_h = 18
        btn_w = 18
        btn_y = status_y + (self.STATUS_HEIGHT - btn_h) // 2
        
        self.scale_down_btn = pygame.Rect(scale_x, btn_y, btn_w, btn_h)
        down_hover = self.scale_down_btn.collidepoint(mouse_pos)
        pygame.draw.rect(self.screen, Theme.ACCENT if down_hover else Theme.BG_PANEL, self.scale_down_btn, border_radius=3)
        minus_text = self.font_small.render("-", True, Theme.TEXT)
        self.screen.blit(minus_text, (self.scale_down_btn.centerx - minus_text.get_width() // 2, self.scale_down_btn.centery - minus_text.get_height() // 2))
        
        self.screen.blit(scale_label, (scale_x + btn_w + 6, status_y + (self.STATUS_HEIGHT - scale_label.get_height()) // 2))
        
        self.scale_up_btn = pygame.Rect(scale_x + btn_w + 6 + scale_label.get_width() + 6, btn_y, btn_w, btn_h)
        up_hover = self.scale_up_btn.collidepoint(mouse_pos)
        pygame.draw.rect(self.screen, Theme.ACCENT if up_hover else Theme.BG_PANEL, self.scale_up_btn, border_radius=3)
        plus_text = self.font_small.render("+", True, Theme.TEXT)
        self.screen.blit(plus_text, (self.scale_up_btn.centerx - plus_text.get_width() // 2, self.scale_up_btn.centery - plus_text.get_height() // 2))
        
        frames_text = self.font_small.render(f"Frames: {self.frame_count}", True, Theme.TEXT_DIM)
        self.screen.blit(frames_text, (win_w - self.PADDING - frames_text.get_width(), status_y + (self.STATUS_HEIGHT - frames_text.get_height()) // 2))
    
    def run(self):
        self.init_display()
        self.refresh_ports()
        self.connect()
        self.running = True
        
        read_thread = threading.Thread(target=self.read_thread, daemon=True)
        read_thread.start()
        
        clock = pygame.time.Clock()
        
        while self.running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    self.running = False
                elif event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        self.running = False
                    elif event.key in (pygame.K_UP, pygame.K_w):
                        self.send_input('up')
                    elif event.key in (pygame.K_DOWN, pygame.K_s):
                        self.send_input('down')
                    elif event.key in (pygame.K_LEFT, pygame.K_a):
                        self.send_input('left')
                    elif event.key in (pygame.K_RIGHT, pygame.K_d):
                        self.send_input('right')
                    elif event.key in (pygame.K_RETURN, pygame.K_SPACE):
                        self.send_input('select')
                elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    if self.close_btn and self.close_btn.collidepoint(event.pos):
                        self.running = False
                    elif self.port_prev_btn and self.port_prev_btn.collidepoint(event.pos):
                        self.select_port(-1)
                    elif self.port_next_btn and self.port_next_btn.collidepoint(event.pos):
                        self.select_port(1)
                    elif self.connect_btn and self.connect_btn.collidepoint(event.pos):
                        self.refresh_ports()
                        self.connect()
                    elif self.disconnect_btn and self.disconnect_btn.collidepoint(event.pos):
                        self.disconnect()
                        self.status_msg = "Disconnected"
                    elif self.swap_btn and self.swap_btn.collidepoint(event.pos):
                        self.swap_bytes = not self.swap_bytes
                        self.pixel_array.fill(0)
                        if self.serial and self.connected:
                            try:
                                self.serial.write(b"mirror refresh\n")
                            except:
                                pass
                    elif self.scale_down_btn and self.scale_down_btn.collidepoint(event.pos):
                        self.change_scale(-1)
                    elif self.scale_up_btn and self.scale_up_btn.collidepoint(event.pos):
                        self.change_scale(1)
                    elif event.pos[1] < self.TITLE_BAR_HEIGHT and not self.close_btn.collidepoint(event.pos):
                        try:
                            import ctypes
                            hwnd = pygame.display.get_wm_info()['window']
                            ctypes.windll.user32.ReleaseCapture()
                            ctypes.windll.user32.SendMessageW(hwnd, 0x0112, 0xF012, 0)
                        except:
                            pass
                    else:
                        for btn in self.buttons:
                            if btn['rect'].collidepoint(event.pos):
                                self.send_input(btn['cmd'])
                                break
            
            if self.connected and self.last_data_time > 0 and time.time() - self.last_data_time > 3:
                self.connected = False
                self.status_msg = "Connection lost"
            
            if self.resize_pending:
                self.resize_display()
            
            while self.frame_queue:
                frame = self.frame_queue.popleft()
                self.process_frame(*frame)
            
            self.draw_gui()
            pygame.display.flip()
            clock.tick(60)
        
        self.disconnect()
        pygame.quit()


def list_ports():
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found")
        return
    print("Available ports:")
    for p in ports:
        print(f"  {p.device}: {p.description}")

def find_ghostesp_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        try:
            s = serial.Serial()
            s.port = p.device
            s.baudrate = 115200
            s.timeout = 0.5
            s.dtr = False
            s.rts = False
            s.open()
            s.reset_input_buffer()
            s.write(b"identify\n")
            time.sleep(0.3)
            response = s.read(256).decode('utf-8', errors='ignore')
            s.close()
            if "GHOSTESP_OK" in response:
                print(f"Found GhostESP on {p.device}")
                return p.device
        except:
            pass
    return None


def main():
    parser = argparse.ArgumentParser(description='GhostESP Screen Mirror')
    parser.add_argument('port', nargs='?', help='Serial port (e.g. COM3, /dev/ttyUSB0)')
    parser.add_argument('--scale', type=int, default=2, help='Display scale factor (default: 2)')
    parser.add_argument('--list', action='store_true', help='List available serial ports')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    
    args = parser.parse_args()
    
    if args.list:
        list_ports()
        return
    
    port = args.port
    if not port:
        print("Searching for GhostESP device...")
        port = find_ghostesp_port()
        if not port:
            ports = serial.tools.list_ports.comports()
            port = ports[0].device if ports else "COM1"
            print(f"No GhostESP found, using {port}")
    
    mirror = GhostMirror(port, args.baud, args.scale)
    mirror.run()


if __name__ == '__main__':
    main()
