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

Do not assume the `6121S07_L8_CTA` suffix is a public driver folder. Current
evidence says it is a Bird project/module variant selecting the generic SP0A09
MTK driver.
