# Goggle 2 boot and runtime performance

Results and findings of the optimization work on branch `perf-runtime`, measured
on one HDZero Goggle 2. Times are seconds since kernel start, from
`BOOT_TIMING.log`. A stopwatch from power-on shows 1-2 s more (boot0, u-boot).

## Results

| Measure | Before | Now |
| --- | --- | --- |
| Boot to first analog signal | 22.8-24.3 s | 6.11-7.20 s |
| Boot to first HDZero signal | 17.8 s | 9.14-9.15 s (last measured before `sd_mount_bg` and `cpu_boost`) |
| VDPO mode set in the driver | 1.08 s | 0.11 s |
| Source switch to signal, analog | ~3.8 s | 0.63-0.86 s |
| Wake from sleep, analog | ~4 s | 0.34-0.46 s |
| Wake from sleep, HDZero | 4.8 s | ~2 s |
| Whole-system CPU in video | 105-127 % of a core at ~1.1 GHz | 6-13 % at 600 MHz |

## What was found

### i2c1 hang on fast boots

`setup_uart` in `rc.sh` rewrites the whole PE16-23 mux register and disables
PE16/17, which are i2c1's pins. The i2c driver runtime suspends 5 s after its
last use, ~6.2-6.3 s into the boot, and only muxes its pins back on resume. So
the write is harmless after that moment. If `rc.sh` gets there earlier, and the
first transfer also comes before it, the driver counts on pins that are off:
every transfer times out, each one restarts the 5 s, the app blocks on the bus
and the goggles stay on the logo. The stock boot is too slow to reach this.

`setup_uart` now keeps those two pins as they are on the G2. Every earlier theory
(the display driver load, the S50usb sleep, USB timing) was a coincidence in time.

Hangs switched off before the 30 s `BOOT_HANG.log` entry leave no trace there.
`dvr_dbg.log` shows them: the record process logs one `[rec] loadConf` per boot
even in a hang, so such an entry with no `PERF.log` session is a hang. A
`[rec] loadConf` right after `[app] update_conf` is a recording start, not a boot.

### The 1.1 s of every VDPO mode set

`disp_vdpo_disable()` in `disp.ko` ends with an unconditional
`disp_delay_ms(1000)`. `bootopt/vdpo_fast_off` patches a RAM copy to 50 ms: the
`mov.w r0, #1000` at file offset 0x12eac, used only if exactly those 4 bytes
differ, bind-mounted while `dispw -x` loads the driver. Nothing is written to flash.

### Fast and slow boots

About one boot in four ran every step ~1.35 times faster. The clock registers
show only the CPU differs: ondemand samples the load every 2 s here
(`clock-latency` 2 ms in the device tree, times 1000), and its first sample, at
~0.8 s, picks 1104 MHz or 816 MHz for the next 2 s. Why that sample differs was
not measured. `bootopt/cpu_boost` holds 1104 MHz from ~2.9 s until the app is up.

The gap between the two kinds of boot stays at ~0.65 s up to `rc.sh`, all of it
before ~2.9 s: kernel 0.06 s, up to the first script 0.19 s, `/mnt/app` mount
0.26 s, UDISK, mdev and S00mpp 0.15 s. An `inittab` hook at ~1.75 s could recover
~0.4 s, but a bad file there can stop the boot with no fallback, so it was not done.

### Waits in the vendor init scripts

- `udevadm settle`: `/usr/bin/udev_handler` retries the mount of the already
  mounted SD card, `usleep(500000)` after each EBUSY, 1 s on every boot.
- S50usb: a fixed `sleep 2` before a USB gadget bind that fails (-19) on every
  boot. Nothing uses the gadget.
- S01app: the SD card mount, 0.4-0.5 s, in the foreground.

## Boot options

Files in `<SD>/bootopt/`, all opt-in, G2 only.

| File | Effect |
| --- | --- |
| `vdpo_fast_off` | VDPO mode set in 0.2 s instead of 1.1 s |
| `udev_settle_bg` | `S10udev.bg`: settle in the background |
| `s50usb_bg` | `S50usb.bg`: sleep and gadget bind in the background |
| `sd_mount_bg` | `S01app.bg`: SD card mount in the background |
| `cpu_boost` | `S00cpuboost`: performance governor until the app is up |
| `analog_direct`, `hdzero_direct` | The first vdpo mode set is already the source's |
| `analog_early_rx` | Early analog receiver power-up |

The script flags are installed on the rootfs overlay after a boot where the app
came up, and work from the next boot. Without the flag, including a boot without
the SD card, the original comes back. If the app does not come up, the original
comes back and the flag becomes `<flag>.failed`.

## Diagnostics kept in the firmware

An empty `BOOT_TIMING.log` on the SD card enables them.

| File | Contents |
| --- | --- |
| `BOOT_TIMING.log` | Report of each boot, wake and switch, with kernel log |
| `PERF.log` | CPU, temperature and battery every 10 s |
| `REG_SNAP.log` | Pin mux, clocks and CPU frequency history of each boot |
| `I2C_HANG.log` | Pin, clock and i2c state, written ~11 s into a hang |
| `BOOT_HANG.log` | System state when the app is not up after ~30 s |
| `BOOT_NOSD.log` | Same for a boot without the SD card, moved to the card later |
| `vendor_dump/` | One-time copy of the kernel, rootfs, u-boot, pinmux and clock tables |

## Do not

- Do not use `reboot -f`: it left the goggles dead until a power cycle.
- Do not rewrite a pin mux register whole on the G2: the i2c driver will not
  notice, see `setup_uart`.
- Do not write the boot or rootfs partitions to save time: the kernel takes
  ~1.1 s, and a mistake means goggles that do not start.

## Not tested

`sd_mount_bg` and `cpu_boost` were tested on analog only, in short sessions: no
HDZero boot with a signal and no flights. Boots without an SD card were not
tested since the i2c1 fix.
