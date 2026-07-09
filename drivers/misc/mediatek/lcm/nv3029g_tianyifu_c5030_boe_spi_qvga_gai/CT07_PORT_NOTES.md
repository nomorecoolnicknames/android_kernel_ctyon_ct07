# ILI9341 CT07 LCM Port Notes

Firmware evidence:

- LK string: `ILI9341_MZ2D4N511_A_CTC_SPI_QVGA`
- Neighbor LK variant: `NV3029G_TIANYIFU_C5030_BOE_SPI_QVGA_GAI`
- Display size: `240x320`
- Boot logo: `qvga`
- `MTK_LCM_DEVICE_TREE_SUPPORT = no`
- Reverse says SPI-like DBI, not MIPI DSI.
- Compare-id anchors: read `0xD9` / `0xD3`, expected `0x9341`.
- Reset/init delay anchors: init reset = `20/150/120 ms` (from LK RE
  2026-07-09, `kernel-reverse/lk-nv3029g-display-20260709.md`). The old
  `20/20/100` anchor was LK's *compare_id* reset, not *init* — corrected.
  Init table also needs delays 5 ms after {0x80,0x05}, 100 ms after
  {0x80,0x01}, 10 ms after 0x29 (all applied, commit ee713a70).
- DTB pinctrl names: `lcd_cs`, `lcd_clk`, `lcd_rs`, `lcd_data`,
  `lcd_backlight`.

Source candidate:

- `source_candidate/ili9341_alcatel_ot903d.c`
- Original raw URL:
  `https://raw.githubusercontent.com/luckasfb/lcm_drivers/27899abfce92ada2ee354f07c825239272cff645/alcatel_ot_903d_jrd73_gb/lcm/ili9341/ili9341.c`

Status:

- This is not a ready target driver.
- Use it as an ILI9341 register/init seed only.
- Build a new MTK 3.18 folder named `ili9341_mz2d4n511_a_ctc_spi_qvga`.
- Adapt bus parameters against the target donor's
  `drivers/misc/mediatek/lcm/inc/lcm_drv.h`.

Do not use donor `r63417_*` panel config for CT07; it is a QHD/FHD DSI donor
panel and conflicts with stock CT07 firmware evidence.
