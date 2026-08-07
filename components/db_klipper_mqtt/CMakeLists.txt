idf_component_register(
    SRCS "db_klipper_mqtt.c"
    INCLUDE_DIRS "include"
    # esp-mqtt to the printer's Moonraker broker; esp_timer for pacing; pb_policy for
    # the lease/command/snapshot API; esp-tls for optional mqtts. Telemetry/RPC JSON is
    # built with snprintf (no cJSON). The pure arming core (db_klipper_mqtt_arm.h) is
    # header-only and host-tested separately.
    REQUIRES nvs_flash mqtt esp_timer pb_policy esp-tls
)
