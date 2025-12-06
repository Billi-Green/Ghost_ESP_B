if(NOT CONFIG_USE_USB_KEYBOARD)
    idf_component_register()
    return()
endif()

idf_component_register(SRCS "hid_host.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES espressif__usb
                       )
