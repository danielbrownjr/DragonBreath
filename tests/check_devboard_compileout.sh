#!/bin/sh
set -eu

build_dir="${1:-build-debug-devboard-native}"
config="$build_dir/config/sdkconfig.h"
nm_tool="${CROSS_COMPILE:-riscv32-esp-elf-}nm"

if ! command -v "$nm_tool" >/dev/null 2>&1; then
    echo "dev-board compile-out check requires $nm_tool in PATH" >&2
    exit 1
fi
if [ ! -f "$config" ]; then
    echo "missing dev-board build config: $config" >&2
    exit 1
fi

grep -q '^#define CONFIG_PB_DEVBOARD_SAFE 1$' "$config"
if ! grep -Eq \
    '^#define CONFIG_ESP_CONSOLE_(USB_SERIAL_JTAG|UART_DEFAULT) 1$' "$config"; then
    echo "safe dev-board build requires a supported serial console" >&2
    exit 1
fi
if grep -q '^#define CONFIG_PB_POWER_LED 1$' "$config"; then
    echo "safe dev-board build must not claim the Panda Power LED" >&2
    exit 1
fi

for component in pb_board pb_heater pb_fan pb_leds pb_buttons; do
    archive="$build_dir/esp-idf/$component/lib$component.a"
    if [ ! -f "$archive" ]; then
        echo "missing safe dev-board component archive: $archive" >&2
        exit 1
    fi
    if ! undefined_symbols=$("$nm_tool" -u "$archive"); then
        echo "$nm_tool failed to inspect $archive" >&2
        exit 1
    fi
    if printf '%s\n' "$undefined_symbols" | grep -Eq \
        'gpio_(config|set_level|get_level|install_isr_service|isr_handler_add)'; then
        echo "$component still references physical GPIO in safe dev-board build" >&2
        exit 1
    fi
done

archive="$build_dir/esp-idf/pb_ntc/libpb_ntc.a"
if [ ! -f "$archive" ]; then
    echo "missing safe dev-board component archive: $archive" >&2
    exit 1
fi
if ! undefined_symbols=$("$nm_tool" -u "$archive"); then
    echo "$nm_tool failed to inspect $archive" >&2
    exit 1
fi
if printf '%s\n' "$undefined_symbols" | grep -Eq 'adc_(oneshot|cali)'; then
    echo "pb_ntc still references the ADC backend in safe dev-board build" >&2
    exit 1
fi

echo "safe dev-board compile-out check: PASS"
