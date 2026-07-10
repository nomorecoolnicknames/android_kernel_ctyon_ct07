# SP0A09 CT07 Port Notes

Source:

- Repo: `https://github.com/MediatekAndroidDevelopers/android_kernel_uhans_h5000`
- Commit: `1cb5056d97d1c964e569ba2433247cb894efa7d4`
- Original path: `drivers/misc/mediatek/imgsensor/src/mt6735m/sp0a09_mipi_raw`

Firmware evidence:

- `CUSTOM_KERNEL_IMGSENSOR = sp0a09_mipi_raw`
- `CUSTOM_KERNEL_MAIN_IMGSENSOR = sp0a09_mipi_raw`
- `BIRD_MAIN_IMGSENSOR = sp0a09_mipi_raw_6121S07_L8_CTA`
- Reverse evidence: active HAL/kernel sensor id `0x0a09`, name `sp0a09mipiraw`.
- DTB main camera node: `camera_main@10`, `reg = <0x10>`.

Port requirements in the target kernel:

- Add `SP0A09_SENSOR_ID 0x0A09`.
- Add `SENSOR_DRVNAME_SP0A09_MIPI_RAW "sp0a09mipiraw"`.
- Add `SP0A09MIPI_RAW_SensorInit` to `kd_sensorlist.h`.
- Compare `kd_camera_hw.c` rails/pins against CT07 DCT/DTB before copying the
  h5000 power branch.

Stock-binary alignment completed 2026-07-09:

- `kdCISModulePowerOn` now has the stock SP0A09-only sequence: VCAMIO 1.8 V
  and VCAMA 2.8 V, no VCAMD/VCAMAF, stock reset/PDN order and delays, and the
  stock active-low PDN table polarity for both populated slots.
- The 188-byte stock `imgsensor_info` object was decoded and applied: checksum,
  high-speed fps, mode count/delays, shutter limits, RAW_R order, address table,
  and I2C speed now match the CT07 `vmlinux.elf` constants.
- The mutable `imgsensor` defaults (mirror, shutter, gain and dummy values) and
  all 46 `sensor_init()` register writes were compared against stock. The four
  differing writes were corrected; the crop/winsize table was already
  byte-identical.
- `SET_ESHUTTER` now matches the stock clamp (`7..0xffff`) and writes the full
  high byte to register `0x03`; the donor's `& 0x07` mask had limited effective
  exposure to `0x07ff`.
- Device-free ELF comparison confirms exact parity for the three static data
  blocks: `imgsensor_info` (188 bytes), mutable `imgsensor` defaults (52
  bytes), and the winsize table (160 bytes). The sensor, power, and lifecycle
  translation units compile together with the expected symbols.

These comparisons establish source-to-stock constant/table parity, not live
camera function. Probe ID `0x0a09`, MIPI frames, Bayer colors, exposure and the
factory checksum test still require the physical CT07.

Do not assume the `6121S07_L8_CTA` suffix is a public driver folder. Current
evidence says it is a Bird project/module variant selecting the generic SP0A09
MTK driver.
