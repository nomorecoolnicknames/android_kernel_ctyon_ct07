# CT07 MIR3DA / DA226 provenance

The CT07 stock DTB declares `mediatek,da226` on I2C bus 2 at 7-bit address
`0x26`, direction 7, with no external regulator. The stock CT07 `vmlinux.elf`
contains the matching `mir3da_*` implementation and the original build path
`drivers/misc/mediatek/accelerometer/mir3da/mir3da_cust.c`.

The initial driver files came from the GPLv2 MT6737 Linux 3.18.19 tree at:

- repository: `https://github.com/SoCXin/MT6737`
- commit: `89eaf42ad72e6fcde9b1179c7be30e05968fe63c`
- source path: `linux/kernel/drivers/misc/mediatek/accelerometer/mir3da/`

That donor variant was not electrically usable on CT07 unchanged. The local
stock-alignment delta is intentionally kept in this directory:

- OF match is `mediatek,gsensor`, matching the compiled CT07 DTB and stock
  `accel_of_match`;
- probe takes the first address from the `mediatek,da226` platform data
  (`0x26`) before the first I2C read, matching stock, while retaining the
  observed `0x27` fallback;
- the donor-only forced 400 kHz client timing is removed;
- `MIR3DA_OFFSET_TEMP_SOLUTION` is disabled because the stock binary has no
  offset-file workqueue path;
- the seven base driver attributes are restored (the stock attribute table is
  exactly seven pointers), including the stock-visible write callbacks;
- byte I2C reads propagate SMBus errors instead of turning NACK/timeouts into
  plausible register values; sample callbacks reject failed reads/parses;
- chip-info read failures propagate out of resume, and failed auto-probe
  unregisters the I2C driver instead of leaving a half-bound callback set;
- probe keeps the stock dual registration: legacy `hwmsen_attach` for the
  shipped vendor HAL fallback plus Android M control/data paths; unwind and
  remove detach the legacy callback before freeing driver state;
- missing-DT, remove, and probe error paths clear client/global pointers and
  no longer dereference NULL or report success after a failed setup.
- the shipped `libgsensor_jni` command `0x8515` is implemented: it samples the
  device and writes the stock-compatible 256-byte `/data/mir3da` calibration
  record in `z x y` order, with bounded/zeroed data and complete error checks.

This is still not a claim of byte-identical stock source. The factory ioctl
used by the shipped JNI has been reconstructed, but live acceptance still
requires a bound I2C client at `0x26`, chip ID `0x13`, changing XYZ samples,
and verification that command `0x8515` creates a valid `/data/mir3da` record.
