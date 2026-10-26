# Round AMOLED 1.75C board — local patch list

`managed_components/` is gitignored, so every local edit the round board
needs is recorded here. Re-apply after re-fetching the BSP component.

## 1. BSP — waveshare__esp32_s3_touch_amoled_1_75c

`esp32_s3_touch_amoled_1_75c.c`, `bsp_display_lcd_init()`:

```c
-            .buffer_height = 50,
+            /* 50 rows needs a 46 KB DMA bounce block in internal RAM; it
+             * fails once Wi-Fi is up and every failed strip skips pixels
+             * (花屏). 4 rows needs 3.7 KB like the 2.16 BSP. */
+            .buffer_height = 4,
```

Root cause of the garbled screen: the esp_lcd SPI driver allocates an
internal DMA bounce buffer of `hor_res * buffer_height * 2` bytes per
flush. At 466 px × 50 rows that is 46,600 bytes — larger than the biggest
free internal block once Wi-Fi/audio are up, so `tx_color` returns
`ESP_ERR_NO_MEM` and every failed strip leaves stale pixels on the glass.

## 2. PMU — new file pager/pager_pmu.c + call in main.cpp

The 1.75C has an AXP2101 (I2C 0x34) in front of the panel/touch/audio
rails; without it nothing powers up. `pager_pmu_init()` checks the chip ID
and enables exactly what the board needs (non-destructive):

- DCDC1 = 3.3 V (system) — register 0x80 bit 0
- ALDO1 = 3.3 V (peripherals) — register 0x90 bit 0

Charge profile: 400 mA CC / 50 mA precharge / 25 mA stop / 4.2 V.
Register map and the enable sequence follow Waveshare's official
`examples/esp-idf/01_AXP2101` example. Call before `bsp_display_start()`.

## 3. Side key polarity — pager_board.h

The round board's side key (GPIO3) idles LOW, so it is active HIGH
(`PAGER_PTT_ACTIVE_LOW 0`). The old 2.16 key on GPIO18 was active low.
GPIO3 also needs the internal pull-DOWN enabled instead of pull-UP.

## 4. QA console — pager_qa.c serial task

`CONFIG_ESP_CONSOLE_UART_CUSTOM=y` routes stdin/stdout to UART0, which is
NOT wired on this board — its USB connector is only the SECONDARY console
(USB-Serial-JTAG). So `getchar()` never sees the bench. The QA serial task
polls the USB-Serial-JTAG RX FIFO directly at register level:

```c
if (usb_serial_jtag_ll_rxfifo_data_available()) {
    uint8_t c;
    usb_serial_jtag_ll_read_rxfifo(&c, 1);
    ...
}
```

Do NOT call `usb_serial_jtag_read_bytes()` here — that needs
`usb_serial_jtag_driver_install()` and deadlocks with the console's own
ownership of the peripheral (device froze at panel init).

## 5. QA screenshot over UART — qa_serial.py decoding

Rows come back as w*4 hex chars per row but USB packet boundaries split
some rows across two lines, and device logs interleave between them.
Decode by stripping every non-hex character, re-cutting the blob into
w*4-char rows, then converting. The snapshot buffer stores RGB565
**byte-swapped** (what the panel wants), so read `(hi << 8) | lo` before
unpacking RGB, or the frame comes out purple/orange.
