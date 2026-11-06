#!/bin/sh

# Boot timing marks: "<seconds since kernel start> <tag>" per line, shell
# builtins only (no forks), into RAM. The app merges them into its report in
# BOOT_TIMING.log on the SD card when that file exists (src/core/boot_timing.c).
BOOT_TIMING_SH_LOG=/tmp/boot_timing_sh.log
function bt()
{
	local up rest
	read up rest < /proc/uptime
	echo "$up $1" >> $BOOT_TIMING_SH_LOG
}
: > $BOOT_TIMING_SH_LOG
bt rc_start

# Is the SD card mounted? Shell builtins only.
function sd_mounted()
{
	local dev mnt rest
	while read dev mnt rest; do
		[ "$mnt" = /mnt/extsd ] && return 0
	done < /proc/mounts
	return 1
}

# Kernel log as of now, before the display driver's chatter overwrites the
# early boot in the small ring buffer: while boot timing is enabled, and on
# boots without an SD card (see BOOT_NOSD.log below).
if [ -e /mnt/extsd/BOOT_TIMING.log ] || ! sd_mounted; then
	dmesg > /tmp/boot_dmesg_rc_start.log
	sd_mounted || echo "rc.sh started without SD card, uptime $(cat /proc/uptime)" > /mnt/UDISK/BOOT_NOSD.log
fi

PLATFORM="$(cat /mnt/app/platform)"

# Physical address read through the same interface as the vendor's awr.
function reg_read()
{
	echo $1 > /sys/class/sunxi_dump/dump
	read REG_VAL < /sys/class/sunxi_dump/dump
}

# Pin mux and i2c clock gate state at the steps before the first i2c1 transfer,
# while boot timing is enabled, for comparing a boot where i2c1 hangs with a
# good one: PE_CFG2, PH_CFG0, PH_CFG1, TWI_BGR. Peripheral registers are left
# out, they may be clock gated here. Shell builtins only.
REG_SNAP=/tmp/reg_snap.log
function reg_snap()
{
	local up rest r line
	[ -e /mnt/extsd/BOOT_TIMING.log ] || return
	read up rest < /proc/uptime
	line="$up $1"
	for r in 0x0300B098 0x0300B0FC 0x0300B100 0x0300191C; do
		reg_read $r
		line="$line $r=$REG_VAL"
	done
	echo "$line" >> $REG_SNAP
}

function insmod_all()
{
	insmod /mnt/app/ko/gpio_keys_hdzero.ko
	insmod /mnt/app/ko/rotary_encoder.ko
}

function setup_uart()
{
	# PE_CFG2 puts uart2 on PE18/19. The vendor value also disables PE16/17,
	# i2c1's pins on the G2. That only works while the i2c driver is runtime
	# suspended: its pins are in the "sleep" state anyway and it muxes them
	# back on the next transfer. It suspends 5 s after its last use
	# (autosuspend_delay_ms), ~6.2-6.3 s into the boot. Before that it counts
	# on the pins being on i2c and never touches them, so if the first
	# transfer also comes before that moment, every i2c1 transfer times out,
	# each one restarts the 5 s, and the app blocks on the bus with the
	# goggles on the logo for the whole boot. The vendor boot never gets here
	# that early, a faster one does (bootopt/udev_settle_bg, or a shorter
	# S50usb). So the two pins stay as the driver left them.
	local pe=0x00775577
	if [ $PLATFORM == "HDZGOGGLE2" ]; then
		reg_read 0x0300B098
		case $REG_VAL in
			0x????????) pe=0x007755${REG_VAL#0x??????} ;;
		esac
	fi
	echo 0x0300B098 $pe > /sys/class/sunxi_dump/write
	echo 0x0300B0D8 0x22777777 > /sys/class/sunxi_dump/write
	echo 0x0300B0fc 0x35517751 > /sys/class/sunxi_dump/write
}

# Inputs of update_boot / write_flashes on the G2 (see those).
function sd_has_flash_files()
{
	[ -e /mnt/extsd/HDZG_BOOT.bin ] || [ -e /mnt/extsd/HDZG_OS.bin ] ||
		[ -e /mnt/extsd/HDZGOGGLE_VA.bin ] || [ -e /mnt/extsd/HDZGOGGLE_RX.bin ]
}

# Boot experiment (opt-in: <SD>/bootopt/analog_direct): the vdpo mode the
# analog source will need when this boot enters it directly (the same choice
# as start_running() in src/core/main.c), so it is set once instead of
# 1080p50 first and the analog mode ~1.1 s later. 1080p50 otherwise.
function boot_vdpo_mode()
{
	local ad=0 hd=0
	[ -e /mnt/extsd/bootopt/analog_direct ] && ad=1
	[ -e /mnt/extsd/bootopt/hdzero_direct ] && hd=1
	if [ $ad = 0 ] && [ $hd = 0 ]; then
		echo 1080p50
		return
	fi
	awk -F= -v ad=$ad -v hd=$hd '
		/^\[/ { sec = $0; next }
		sec == "[autoscan]" && $1 == "status" { st = $2 }
		sec == "[autoscan]" && $1 == "source" { src = $2 }
		sec == "[autoscan]" && $1 == "last_source" { last = $2 }
		sec == "[autoscan]" && $1 == "load_from_boot" { lfb = $2 }
		sec == "[source]" && $1 == "analog_format" { fmt = $2 }
		sec == "[source]" && $1 == "hdz_cam_mode" { cam = $2 }
		END {
			# Keys missing from setting.ini take the defaults of the app
			# (g_setting_defaults): Auto Scan On, source HDZero, NTSC.
			# The G2 never writes analog_format.
			if (st == "") st = 0
			if (src == "") src = 1
			if (last == "") last = 0
			if (fmt == "") fmt = 0
			s = (src == 0) ? last : src
			scan = (st == 0 && lfb != "true")
			if (ad && (s == 3 || (s == 2 && !scan))) {
				print (fmt == 1 ? "720p50" : "720p60")
			} else if (hd && s == 1 && (st == 1 || (st == 0 && lfb == "true"))) {
				# HDZero entered directly: the timing of the saved camera
				# mode (hdz_cam_mode, src/driver/hardware-goggle2.c),
				# 720p60 for the default camera mode when not saved yet.
				if (cam == 3 || cam == 6)
					print "720p90"
				else if (cam == 7 || cam == 8)
					print "1080p60"
				else
					print "720p60"
			} else {
				print "1080p50"
			}
		}' /mnt/app/setting.ini 2>/dev/null || echo 1080p50
}

function config_uart_vdpo()
{
	setup_uart
	reg_snap after_setup_uart
	insmod_all
	reg_snap after_insmod_all

	# G2: setting the vdpo mode takes ~1.5 s in the driver. It runs in the
	# background (start_vdpo_bg, once the sensor drivers are in) so the app
	# loads meanwhile; the app waits for /tmp/vdpo_pending to go away before
	# it touches the display or I2C (main.c). Not on a boot that flashes
	# boot/FPGA/RX images from the SD card.
	if [ $PLATFORM == "HDZGOGGLE2" ] && ! sd_has_flash_files; then
		VDPO_BG_MODE=$(boot_vdpo_mode)
		echo $VDPO_BG_MODE > /tmp/vdpo_mode
		: > /tmp/vdpo_pending
		return
	fi

	dispw -x

	if [ $PLATFORM == "HDZGOGGLE" ]; then
		dispw -s vdpo 1080p50
	elif [ $PLATFORM == "HDZGOGGLE2" ]; then
		dispw -s vdpo 1080p50
	elif [ $PLATFORM == "HDZBOXPRO" ]; then
		dispw -s vdpo 720p60
	fi
}

# Boot experiment (G2, opt-in: <SD>/bootopt/vdpo_fast_off). Every vdpo mode
# set (`dispw -s`, at boot and on every source or menu switch) spends ~1.1 s in
# the display driver: disp_vdpo_disable() in disp.ko ends with an unconditional
# disp_delay_ms(1000) (decompiled; disp_tv_disable has the same 1000 ms), and
# the kernel log shows 1.11-1.13 s from "vdpo_disable ... disabled 0" to the
# next enable on every set. The app blanks the panel and gates the FPGA input
# (reg 0x8C) around each set. With the flag, a RAM copy of disp.ko has that
# delay cut to 50 ms (`mov.w r0, #1000` at file offset 0x12eac becomes
# `movw r0, #50`) and is bind-mounted over the original while `dispw -x` loads
# the driver. It is used only if it differs from the original in exactly those
# 4 bytes. Nothing is written to flash, so removing the flag restores the stock
# driver. If the app did not come up on a boot that used it, the flag is
# renamed to vdpo_fast_off.failed (deferred section).
DISP_KO=/mnt/app/ko/disp.ko
DISP_FAST_KO=/tmp/disp_fastoff.ko
DISP_FAST_FLAG=/mnt/extsd/bootopt/vdpo_fast_off
function disp_fast_off_mount()
{
	[ $PLATFORM == "HDZGOGGLE2" ] && [ -e $DISP_FAST_FLAG ] || return 1
	# the disp.ko this was worked out on (size, and the 4 bytes below)
	if [ "$(wc -c < $DISP_KO)" != 350640 ]; then
		bt vdpo_fast_off_unknown_disp_ko
		return 1
	fi
	cat $DISP_KO > $DISP_FAST_KO &&
		printf '\100\362\062\000' | dd of=$DISP_FAST_KO bs=1 seek=77484 conv=notrunc 2>/dev/null
	# cmp -l: 1-based offset, then both bytes in octal
	if [ "$(cmp -l $DISP_KO $DISP_FAST_KO 2>&1 | awk '{ s = s $1 " " $2 " " $3 " " } END { print s }')" != \
		"77485 117 100 77486 364 362 77487 172 62 77488 160 0 " ]; then
		bt vdpo_fast_off_patch_failed
		rm -f $DISP_FAST_KO
		return 1
	fi
	if ! mount -o bind $DISP_FAST_KO $DISP_KO; then
		bt vdpo_fast_off_mount_failed
		rm -f $DISP_FAST_KO
		return 1
	fi
	: > /tmp/vdpo_fast_off_used
	bt vdpo_fast_off_on
}

# The G2 vdpo mode set of config_uart_vdpo, in the background. It starts only
# once the sensor insmods are done, and the app's first I2C access waits for
# vdpo_ready. That ordering was a suspected fix for the i2c1 hang, whose cause
# turned out to be in setup_uart; it costs ~0.2 s.
function start_vdpo_bg()
{
	(
		fast=
		disp_fast_off_mount && fast=1
		dispw -x
		bt vdpo_x
		# the driver is loaded; the original file goes back in place
		[ -n "$fast" ] && umount $DISP_KO && rm -f $DISP_FAST_KO
		dispw -s vdpo $1
		rm -f /tmp/vdpo_pending
		bt vdpo_set_$1
	) &
}

function write_flashes()
{
	/mnt/app/script/write_flashes.sh
}

# WORKAROUND (remove once the rootfs ships a fixed hwclock):
# /usr/sbin/hwclock is util-linux 2.25.2 and crashes with SIGSEGV whenever it
# decides the RTC is kept in UTC -- on that path it calls
# setenv("TZ", getenv("TZUTC"), 1), TZUTC is never set, and musl's setenv()
# runs strlen(NULL). Without /etc/adjtime it always picks UTC, so every plain
# "hwclock" call dies, including the "hwclock -w" that /etc/init.d/S01app runs
# at shutdown, and each one leaves a bogus "fatal signal 11" line in kmsg.
# Declaring LOCAL steers it onto the path that works. That is accurate here:
# there is no /etc/localtime and no TZ, so libc local time IS UTC, which is
# what the app writes into the RTC. Delete this function and its call to undo.
function seed_adjtime()
{
	if [ ! -e /etc/adjtime ]; then
		printf "0.000000 0 0.000000\n0\nLOCAL\n" > /etc/adjtime
	fi
}

# Everything that shows why i2c1 does not respond: who owns its pins and in
# which state, the mux and clock registers, whether its interrupt ever fired.
function i2c1_hang_capture()
{
	local f r
	echo "===== i2c1 hang, uptime: $(cat /proc/uptime)"
	cat $BOOT_TIMING_SH_LOG
	echo "--- register snapshots of this boot"; cat $REG_SNAP
	echo "--- pin and clock registers now"
	for r in 0x0300B090 0x0300B094 0x0300B098 0x0300B09C 0x0300B0A0 0x0300B0A4 \
		0x0300B0A8 0x0300B0AC 0x0300B0B0 0x0300B0FC 0x0300B100 0x0300B104 \
		0x0300B10C 0x0300B110 0x0300B114 0x0300B118 0x0300B11C 0x0300191C; do
		reg_read $r
		echo "$r $REG_VAL"
	done
	echo "--- /proc/interrupts"; cat /proc/interrupts
	mount -t debugfs none /sys/kernel/debug
	for f in /sys/kernel/debug/pinctrl/pinctrl-handles \
		/sys/kernel/debug/pinctrl/*/pinmux-pins \
		/sys/kernel/debug/pinctrl/*/pinconf-pins; do
		echo "--- $f"; cat $f
	done
	echo "--- clocks"; grep -iE "twi|apb2|psi|usb" /sys/kernel/debug/clk/clk_summary
	echo "--- i2c adapters"; ls -l /sys/class/i2c-adapter
	for f in /sys/class/i2c-adapter/i2c-1/device/power/*; do
		echo "$f: $(cat $f)"
	done
	echo "--- dmesg"; dmesg
}

# Read last: the controller may be clock gated, and in the worst case this
# never returns, after the capture above is safe on the card.
function i2c1_hang_twi_regs()
{
	local r
	echo "--- TWI1 registers: ADDR XADDR DATA CNTR STAT CCR SRST EFR LCR"
	for r in 00 04 08 0C 10 14 18 1C 20; do
		reg_read 0x050024$r
		echo "0x050024$r $REG_VAL"
	done
}

# Called when the mcp3021 insmod took seconds instead of milliseconds (see
# setup_uart): keeps the evidence right away, a boot switched off before the
# 30 s BOOT_HANG.log entry otherwise leaves none. No restart from here: reboot
# -f left the goggles dead until a power cycle.
function i2c1_hang_log()
{
	local log=/mnt/UDISK/I2C_HANG.log
	[ $PLATFORM == "HDZGOGGLE2" ] || return
	bt i2c1_hang_detected
	: > /tmp/i2c1_hang
	sd_mounted && log=/mnt/extsd/I2C_HANG.log
	i2c1_hang_capture >> $log 2>&1
	sync
	i2c1_hang_twi_regs >> $log 2>&1
	sync
}

function beep()
{
	echo "131">/sys/class/gpio/export
	echo "1">/sys/class/gpio/gpio131/value
	sleep 1
	echo "0">/sys/class/gpio/gpio131/value
}

function update_boot()
{
	if [ $PLATFORM == "HDZGOGGLE" ]; then
        local boot_file=/mnt/extsd/HDZG_BOOT.bin
        local os_file=/mnt/extsd/HDZG_OS.bin
    elif [ $PLATFORM == "HDZGOGGLE2" ]; then
        local boot_file=/mnt/extsd/HDZG_BOOT.bin
        local os_file=/mnt/extsd/HDZG_OS.bin
	elif [ $PLATFORM == "HDZBOXPRO" ]; then
        local boot_file=/mnt/extsd/HDZB_BOOT.bin
        local os_file=/mnt/extsd/HDZB_OS.bin
	fi

	if [ -e $boot_file ]; then
		export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/mnt/app
		/mnt/app/ota-burnuboot $boot_file
		rm $boot_file
		fw_setenv hdzero_update "sprite_update"
		fw_setenv bootcmd "run hdzero_update setargs_nor boot_normal"
		beep
	elif [ -e $os_file ]; then
		rm $os_file
		beep
	fi
}

echo "0 4 1 7">/proc/sys/kernel/printk

#give hwclock a mode it can survive (see seed_adjtime)
seed_adjtime

reg_snap before_setup_uart

#config UART and VDPO
config_uart_vdpo
bt config_uart_vdpo

#check boot&os file, update if exist
update_boot
bt update_boot

#auto write HDZERO_TX.bin / HDZERO_RX.bin /HDZERO_VA.bin when bootup if they are exist on SD card
write_flashes
bt write_flashes

#load driver
reg_snap before_mcp3021
read insmod_up rest < /proc/uptime
insmod /mnt/app/ko/mcp3021.ko
read insmod_end rest < /proc/uptime
[ $((${insmod_end%%.*} - ${insmod_up%%.*})) -ge 3 ] && i2c1_hang_log
usleep 2000
insmod /mnt/app/ko/nct75.ko
usleep 2000
# The app reads the battery ADC through this node right at startup
# (mcp3021_init). Wait for it instead of the old unconditional "sleep 1"
# before the insmods; capped at 1 s, so the worst case is the old timing.
i=0
while [ ! -e /sys/bus/iio/devices/iio:device0 ] && [ $i -lt 50 ]; do
	usleep 20000
	i=$((i + 1))
done
bt sensor_drivers

# G2: the vdpo mode set, now that no sensor insmod uses i2c1 (start_vdpo_bg)
[ -n "$VDPO_BG_MODE" ] && start_vdpo_bg $VDPO_BG_MODE

#set Microphone Bias Control Register
aww 0x050967c0 0x110e6100

# Set the internal log dir aside: the previous boot's logs are copied to the
# SD card in the background once the app is up (sdstat_log_backup.sh), and the
# record process logs this boot into a fresh /mnt/app/log. If an earlier
# backup never ran (no SD card), keep its set and let it go first.
if [ -d /mnt/app/log ] && [ ! -e /mnt/app/log.prev ]; then
	mv /mnt/app/log /mnt/app/log.prev
fi
[ -d /mnt/app/log ] || mkdir /mnt/app/log

#record process
# record-env.sh also sets LD_LIBRARY_PATH, which the app needs to find its
# MPP libraries, so it is sourced here whether or not record starts now.
source /mnt/app//app/record/record-env.sh
function start_record()
{
	if [ -e /mnt/extsd/RECORD.log ]; then
		/mnt/app/app/record/record > /mnt/extsd/RECORD.log 2>&1 &
	else
		/mnt/app/app/record/record &
	fi
	bt record_started
}
start_record

	#system led
if [ $PLATFORM == "HDZGOGGLE" ]; then
	/mnt/app/script/system_daemon.sh &
elif [ $PLATFORM == "HDZGOGGLE2" ]; then
	/mnt/app/script/system_daemon.sh &
fi

# /etc/init.d/S50usb stays the vendor original (read-only copy in /squashfs).
# It loads the USB drivers, starts adbd and sleeps a fixed 2 s before binding
# the gadget. Two test builds cut that wait through the rootfs overlay,
# boottiming3 (start() backgrounded) and perf10 (only the sleep skipped), and
# both hung i2c1 on the first boot this script started early (see
# setup_uart). S50usb stays unchanged. Undo such an edit wherever it is still
# present: copy the
# original back over the overlay copy (mv over it, never rm: that leaves a
# whiteout).
S50USB=/etc/init.d/S50usb
S50USB_ORIG=/squashfs/etc/init.d/S50usb
function s50usb_heal()
{
	[ -f $S50USB_ORIG ] || return
	cmp -s $S50USB_ORIG $S50USB && return
	cat $S50USB_ORIG > $S50USB.tmp && chmod 755 $S50USB.tmp &&
		mv $S50USB.tmp $S50USB && sync && bt s50usb_restored
	rm -f $S50USB.tmp
}

# Boot experiment (G2, opt-in: <SD>/bootopt/udev_settle_bg). The vendor
# /etc/init.d/S10udev waits for udev to finish its coldplug ("udevadm settle"),
# and on every measured boot that wait ends ~1.06 s after the last coldplug
# event, the SD card's, whose rule runs the vendor /usr/bin/udev_handler; every
# later init script and this one wait for it. S10udev.bg runs that wait in the
# background. Installed on the rootfs overlay (takes effect from the
# next boot) only over the exact original (S10udev.vendor), and only after a
# boot where the app came up. Without the flag the original goes back. If the
# app did not come up on a boot that ran the edited script, the original goes
# back and the flag is renamed to udev_settle_bg.failed.
S10UDEV=/etc/init.d/S10udev
S10UDEV_ORIG=/squashfs/etc/init.d/S10udev
S10UDEV_BG=/mnt/app/script/S10udev.bg
S10UDEV_FLAG=/mnt/extsd/bootopt/udev_settle_bg
function s10udev_update()
{
	local want=$S10UDEV_ORIG
	[ $PLATFORM == "HDZGOGGLE2" ] && [ -f $S10UDEV_ORIG ] && [ -f $S10UDEV_BG ] || return
	if cmp -s $S10UDEV_BG $S10UDEV; then
		bt s10udev_bg_active
		if [ ! -e /tmp/app_init_done ] && [ -e $S10UDEV_FLAG ]; then
			echo "set by rc.sh: the app was not up ~30 s after rc.sh started, with the edited S10udev; uptime $(cat /proc/uptime)" \
				> $S10UDEV_FLAG.failed
			rm -f $S10UDEV_FLAG
		fi
	fi
	if [ -e $S10UDEV_FLAG ] && [ -e /tmp/app_init_done ]; then
		if cmp -s $S10UDEV_ORIG /mnt/app/script/S10udev.vendor; then
			want=$S10UDEV_BG
		else
			bt s10udev_vendor_script_differs
		fi
	fi
	cmp -s $want $S10UDEV && return
	cat $want > $S10UDEV.tmp && chmod 755 $S10UDEV.tmp &&
		mv $S10UDEV.tmp $S10UDEV && sync && bt s10udev_installed_${want##*/}
	rm -f $S10UDEV.tmp
}

# One-time copy of vendor files for offline analysis, while boot timing is
# enabled, into <SD>/vendor_dump (made again only when that folder is
# deleted): the kernel and rootfs (squashfs) partitions, which hold the closed
# dispw and udev_handler, and the partition and module tables. Read-only.
function vendor_dump()
{
	local d=/mnt/extsd/vendor_dump
	sd_mounted && [ -e /mnt/extsd/BOOT_TIMING.log ] && [ ! -e $d ] || return
	rm -rf $d.tmp
	mkdir $d.tmp || return
	{
		cat /proc/mtd > $d.tmp/proc_mtd.txt
		cat /proc/modules > $d.tmp/proc_modules.txt
		cat /proc/version > $d.tmp/proc_version.txt
		ls -l /dev/by-name/ > $d.tmp/by-name.txt
		cat /squashfs/usr/sbin/dispw > $d.tmp/dispw
		cat /squashfs/usr/bin/udev_handler > $d.tmp/udev_handler
		[ -e /proc/config.gz ] && cat /proc/config.gz > $d.tmp/config.gz
		cat /dev/by-name/boot > $d.tmp/boot.img
		cat /dev/by-name/rootfs > $d.tmp/rootfs.img
	} 2>> $d.tmp/errors.txt
	sync
	mv $d.tmp $d && sync && bt vendor_dump_done
}

# Same place, once, with the app up (debugfs is mounted by then): the baseline
# of what i2c1_hang_capture records, and what the first dump lacked.
function vendor_dump_pins()
{
	local d=/mnt/extsd/vendor_dump
	[ -e /tmp/app_init_done ] && [ -d $d ] && [ ! -e $d/pinmux-pins.txt ] || return
	{
		cat /sys/kernel/debug/pinctrl/pinctrl-handles > $d/pinctrl-handles.txt
		cat /sys/kernel/debug/pinctrl/*/pinconf-pins > $d/pinconf-pins.txt
		cat /proc/interrupts > $d/proc_interrupts.txt
		cat /sys/kernel/debug/clk/clk_summary > $d/clk_summary.txt
		ls -l /sys/class/i2c-adapter > $d/i2c-adapters.txt
		cat /dev/mtdblock0 > $d/uboot.img
		tar cf $d/device-tree.tar -C /proc device-tree
		cat /sys/kernel/debug/pinctrl/*/pinmux-pins > $d/pinmux-pins.txt
	} 2>> $d/errors2.txt
	sync
	bt vendor_dump_pins_done
}

# Deferred work: start it once the app has finished its bring-up (the app
# creates /tmp/app_init_done), so it doesn't compete with it; at most ~30 s.
(
	i=0
	while [ ! -e /tmp/app_init_done ] && [ $i -lt 150 ]; do
		usleep 200000
		i=$((i + 1))
	done
	bt deferred_start

	# early, so its marks make it into this boot's BOOT_TIMING.log report
	s10udev_update

	if [ -e $REG_SNAP ] && sd_mounted; then
		{ echo "===== boot"; cat $BOOT_TIMING_SH_LOG $REG_SNAP; } >> /mnt/extsd/REG_SNAP.log
	fi
	if [ -e /mnt/UDISK/I2C_HANG.log ] && sd_mounted; then
		cat /mnt/UDISK/I2C_HANG.log >> /mnt/extsd/I2C_HANG.log && rm -f /mnt/UDISK/I2C_HANG.log
	fi

	# vdpo_fast_off: the app did not come up on a boot that used the patched
	# driver (disp_fast_off_mount). An i2c1 hang comes before the driver is
	# even loaded and says nothing about the patch.
	if [ -e /tmp/vdpo_fast_off_used ] && [ ! -e /tmp/app_init_done ] &&
		[ ! -e /tmp/i2c1_hang ] && [ -e $DISP_FAST_FLAG ]; then
		echo "set by rc.sh: the app was not up ~30 s after rc.sh started, with the patched disp.ko; uptime $(cat /proc/uptime)" \
			> $DISP_FAST_FLAG.failed
		rm -f $DISP_FAST_FLAG
		sync
	fi

	# Boot without an SD card: keep a snapshot on the internal UDISK
	# partition; the next boot with a card moves it to BOOT_NOSD.log there.
	if ! sd_mounted; then
		{
			echo "===== boot without SD card, uptime: $(cat /proc/uptime)"
			echo "--- rc.sh marks"; cat /tmp/boot_timing_sh.log
			if [ -e /tmp/app_init_done ]; then echo "--- app init: done"; else echo "--- app init: NOT done"; fi
			echo "--- /tmp"; ls /tmp
			echo "--- ps"; ps
			for p in $(pidof HDZGOGGLE record); do
				echo "--- threads of $p: name state wchan"
				for t in /proc/$p/task/*; do
					echo "$(cat $t/comm) $(cut -d' ' -f3 $t/stat) $(cat $t/wchan)"
				done
			done
			echo "--- dmesg at rc.sh start"; cat /tmp/boot_dmesg_rc_start.log
			echo "--- dmesg now"; dmesg
		} >> /mnt/UDISK/BOOT_NOSD.log 2>&1
		sync
	elif [ -e /mnt/UDISK/BOOT_NOSD.log ]; then
		mv /mnt/UDISK/BOOT_NOSD.log /mnt/extsd/BOOT_NOSD.log && sync
	fi

	# The app did not finish its bring-up in time: if boot timing is enabled,
	# keep what the system looked like, for diagnosis.
	if [ ! -e /tmp/app_init_done ] && [ -e /mnt/extsd/BOOT_TIMING.log ]; then
		{
			echo "===== app not up after ~30 s, uptime: $(cat /proc/uptime)"
			cat /tmp/boot_timing_sh.log
			ls /tmp
			ps
			dmesg
		} >> /mnt/extsd/BOOT_HANG.log 2>&1
		sync
	fi

	/mnt/app/script/sdstat_log_backup.sh /mnt/app/log.prev

	#external services
	if [ ! -z $(ls -1 /mnt/extsd/hdzgoggle-services-* 2> /dev/null) ]; then
		if [ -e /mnt/extsd/SERVICES.log ]; then
			/mnt/app/services/install.sh > /mnt/extsd/SERVICES.log 2>&1 &
		else
			/mnt/app/services/install.sh &
		fi
	elif [ -e /mnt/app/services/startup.sh ]; then
		if [ -e /mnt/extsd/SERVICES.log ]; then
			/mnt/app/services/startup.sh > /mnt/extsd/SERVICES.log 2>&1 &
		else
			/mnt/app/services/startup.sh &
		fi
	fi

	s50usb_heal
	bt deferred_end

	vendor_dump
	vendor_dump_pins
) &
#start applicaion
bt app_launch
if [ -e /mnt/extsd/develop.sh ]; then
	/mnt/extsd/develop.sh
else
	if [ -e /mnt/extsd/HDZGOGGLE.log ]; then
		/mnt/app/app/HDZGOGGLE > /mnt/extsd/HDZGOGGLE.log 2>&1 &
	else
		/mnt/app/app/HDZGOGGLE &
	fi
fi
bt rc_end
