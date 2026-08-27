idf_component_register(
    SRCS "pb_heater.c" "pb_heater_algorithm_store.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board pb_ntc dc_pid driver esp_timer nvs_flash
)
