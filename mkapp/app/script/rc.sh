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

function insmod_all()
{
	insmod /mnt/app/ko/gpio_keys_hdzero.ko
	insmod /mnt/app/ko/rotary_encoder.ko
}

function setup_uart()
{
	echo 0x0300B098 0x00775577 > /sys/class/sunxi_dump/write
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
	insmod_all

	# G2: setting the vdpo mode takes ~1.5 s in the driver. Do it in the
	# background so the app loads meanwhile; the app waits for
	# /tmp/vdpo_pending to go away before it touches the display (main.c).
	# Not on a boot that flashes boot/FPGA/RX images from the SD card.
	if [ $PLATFORM == "HDZGOGGLE2" ] && ! sd_has_flash_files; then
		local mode=$(boot_vdpo_mode)
		echo $mode > /tmp/vdpo_mode
		: > /tmp/vdpo_pending
		(
			dispw -x
			bt vdpo_x
			dispw -s vdpo $mode
			rm -f /tmp/vdpo_pending
			bt vdpo_set_$mode
		) &
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
insmod /mnt/app/ko/mcp3021.ko
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

# /etc/init.d/S50usb must stay the vendor original (read-only copy in
# /squashfs). It loads the USB drivers, starts adbd and sleeps a fixed 2 s
# before binding the gadget, and that sleep is what keeps i2c1 safe: the USB
# drivers keep working for a while after their insmod, and an i2c1 access in
# that window hangs the bus for the whole boot (xfer timeouts, app never up).
# Two test builds cut the wait through the rootfs overlay and both hit it on
# the first boot this script reached its sensor insmods (mcp3021) early:
# boottiming3 (start() backgrounded) and perf10 (insmods kept in order, only
# the sleep skipped). Undo such an edit wherever it is still present: copy the
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
