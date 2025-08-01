from datetime import datetime

def timestamp(fmt="%Y-%m-%d %H:%M:%S"):
    """Return the current timestamp as a string."""
    return datetime.now().strftime(fmt)

def log_message(log_widget, message):
    from datetime import datetime
    timestamp = datetime.now().strftime("%H:%M:%S")
    log_widget.append(f"[{timestamp}] {message}")