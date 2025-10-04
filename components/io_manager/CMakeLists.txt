idf_component_register(
    SRCS "io_manager.c" "i2c_bus_lock.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
