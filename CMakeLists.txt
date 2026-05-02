if(CONFIG_ZMK_ESP32_STATUS_UART)
  zephyr_library()
  zephyr_library_sources(src/esp32_status_uart.c)
endif()
