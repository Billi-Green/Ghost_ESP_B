#ifndef USB_KEYBOARD_MANAGER_H
#define USB_KEYBOARD_MANAGER_H

#include <stdbool.h>

void usb_keyboard_manager_init(void);
bool usb_keyboard_manager_is_host_mode(void);
void usb_keyboard_manager_set_host_mode(bool enable);
void usb_keyboard_manager_register_stream_handler(void);

#endif
