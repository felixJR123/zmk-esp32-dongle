if(CONFIG_ZMK_ESP32_STATUS_UART)
  if(TARGET app)
    # Add directly to the `app` target instead of a separate zephyr_library().
    # ZMK's own core listeners (hid_listener.c, keymap.c, ...) are added to
    # `app` from zmk/app/CMakeLists.txt *after* west modules are processed,
    # so this places our ZMK_SUBSCRIPTION entries earlier in the
    # `.event_subscription` linker section than ZMK's. That guarantees our
    # esp32_status_listener always runs before hid_listener, so returning
    # ZMK_EV_EVENT_HANDLED during text-input mode reliably stops the
    # keypress from ever reaching hid_listener/the host — a module built as
    # a separate zephyr_library() is linked in later and cannot depend on
    # running first.
    target_sources(app PRIVATE src/esp32_status_uart.c)
  else()
    zephyr_library()
    zephyr_library_sources(src/esp32_status_uart.c)
  endif()
endif()
