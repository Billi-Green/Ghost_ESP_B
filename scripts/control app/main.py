import sys
from PyQt6.QtWidgets import QApplication
from gui import ESP32ControlGUI

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = ESP32ControlGUI()
    window.show()
    sys.exit(app.exec())