#! /vendor/bin/sh

exec > /dev/kmsg 2>&1

echo "execprog: sh execution"

BIND=/vendor/bin/init.kernel.post_boot.sh

if ! mount | grep -q "$BIND" && [ ! -e /sbin/recovery ] && [ ! -e /dev/ep/.post_boot ]; then
  echo "execprog: restarting under tmpfs"
  mkdir /dev/ep
  mount -t tmpfs nodev /dev/ep
  touch /dev/ep/.post_boot
  cp -p /dev/execprog /dev/ep/execprog
  rm /dev/execprog
  chown root:shell /dev/ep/execprog

  mount --bind /dev/ep/execprog "$BIND"
  chcon "u:object_r:vendor_file:s0" "$BIND"

  umount -l /dev/ep

  echo "97" > /sys/fs/selinux/enforce

  exit
fi

ddr_type=`od -An -tx /proc/device-tree/memory/ddr_device_type`
ddr_type4="07"
ddr_type5="08"
ddr_id=
for _b in `od -An -tx1 /proc/device-tree/memory/ddr_device_type 2>/dev/null`; do ddr_id=$_b; done
case "$ddr_id" in 07|08) ;; *) ddr_id=${ddr_type:4:2} ;; esac

echo 0-3 > /dev/cpuset/background/cpus
echo 0-3 > /dev/cpuset/system-background/cpus

RATE_LIMIT_US=500
set_gov() {
	_gp=/sys/devices/system/cpu/cpufreq/policy$1
	[ -d $_gp ] || return
	echo schedutil > $_gp/scaling_governor
	[ -e $_gp/schedutil/rate_limit_us ] && echo $RATE_LIMIT_US > $_gp/schedutil/rate_limit_us
	echo $2 > $_gp/scaling_min_freq
}
set_gov 0 691200
set_gov 4 691200
set_gov 7 806400

for c0_rimps_l3 in /sys/devices/system/cpu/memlat/c0_memlat/cpu0-cpu-l3-lat
do
	cat $c0_rimps_l3/available_frequencies | cut -d " " -f 1 > $c0_rimps_l3/min_freq
	echo 400 > $c0_rimps_l3/ratio_ceil
	echo 3 > $c0_rimps_l3/sample_ms
done

for c4_rimps_l3 in /sys/devices/system/cpu/memlat/c4_memlat/cpu4-cpu-l3-lat
do
	cat $c4_rimps_l3/available_frequencies | cut -d " " -f 1 > $c4_rimps_l3/min_freq
	echo 4000 > $c4_rimps_l3/ratio_ceil
	echo 3 > $c4_rimps_l3/sample_ms
	echo 60 > $c4_rimps_l3/l2wb_pct
	echo 25000 > $c4_rimps_l3/l2wb_filter
done

for c7_rimps_l3 in /sys/devices/system/cpu/memlat/c7_memlat/cpu7-cpu-l3-lat
do
	cat $c7_rimps_l3/available_frequencies | cut -d " " -f 1 > $c7_rimps_l3/min_freq
	echo 20000 > $c7_rimps_l3/ratio_ceil
	echo 3 > $c7_rimps_l3/sample_ms
	echo 60 > $c7_rimps_l3/l2wb_pct
	echo 25000 > $c7_rimps_l3/l2wb_filter
done

for device in /sys/devices/platform/soc
do
	for cpubw in $device/*cpu-cpu-llcc-bw/devfreq/*cpu-cpu-llcc-bw
	do
		cat $cpubw/available_frequencies | cut -d " " -f 1 > $cpubw/min_freq
		echo "2288 4577 7110 9155 12298 14236 15258" > $cpubw/bw_hwmon/mbps_zones
		echo 4 > $cpubw/bw_hwmon/sample_ms
		echo 68 > $cpubw/bw_hwmon/io_percent
		echo 20 > $cpubw/bw_hwmon/hist_memory
		echo 0 > $cpubw/bw_hwmon/hyst_length
		echo 80 > $cpubw/bw_hwmon/down_thres
		echo 0 > $cpubw/bw_hwmon/guard_band_mbps
		echo 250 > $cpubw/bw_hwmon/up_scale
		echo 1600 > $cpubw/bw_hwmon/idle_mbps
		echo 40 > $cpubw/polling_interval
	done

	for llccbw in $device/*cpu-llcc-ddr-bw/devfreq/*cpu-llcc-ddr-bw
	do
		cat $llccbw/available_frequencies | cut -d " " -f 1 > $llccbw/min_freq
		if [ "$ddr_id" = "$ddr_type4" ]; then
			echo "1144 1720 2086 2929 3879 5931 6515 8136" > $llccbw/bw_hwmon/mbps_zones
		elif [ "$ddr_id" = "$ddr_type5" ]; then
			echo "1144 1720 2086 2929 3879 5931 6515 7980 12191" > $llccbw/bw_hwmon/mbps_zones
		fi
		echo 4 > $llccbw/bw_hwmon/sample_ms
		echo 68 > $llccbw/bw_hwmon/io_percent
		echo 20 > $llccbw/bw_hwmon/hist_memory
		echo 0 > $llccbw/bw_hwmon/hyst_length
		echo 80 > $llccbw/bw_hwmon/down_thres
		echo 0 > $llccbw/bw_hwmon/guard_band_mbps
		echo 250 > $llccbw/bw_hwmon/up_scale
		echo 1600 > $llccbw/bw_hwmon/idle_mbps
		echo 48 > $llccbw/polling_interval
	done

	for l3bw in $device/*snoop-l3-bw/devfreq/*snoop-l3-bw
	do
		cat $l3bw/available_frequencies | cut -d " " -f 1 > $l3bw/min_freq
		echo 4 > $l3bw/bw_hwmon/sample_ms
		echo 10 > $l3bw/bw_hwmon/io_percent
		echo 20 > $l3bw/bw_hwmon/hist_memory
		echo 10 > $l3bw/bw_hwmon/hyst_length
		echo 0 > $l3bw/bw_hwmon/down_thres
		echo 0 > $l3bw/bw_hwmon/guard_band_mbps
		echo 0 > $l3bw/bw_hwmon/up_scale
		echo 1600 > $l3bw/bw_hwmon/idle_mbps
		echo 9155 > $l3bw/max_freq
		echo 40 > $l3bw/polling_interval
	done

	for memlat in $device/*lat/devfreq/*lat
	do
		cat $memlat/available_frequencies | cut -d " " -f 1 > $memlat/min_freq
		echo 8 > $memlat/polling_interval
		echo 400 > $memlat/mem_latency/ratio_ceil
	done

	for latfloor in $device/*cpu0-cpu*latfloor/devfreq/*cpu0-cpu*latfloor
	do
		cat $latfloor/available_frequencies | cut -d " " -f 1 > $latfloor/min_freq
		echo 8 > $latfloor/polling_interval
	done

	for latfloor in $device/*cpu4-cpu*latfloor/devfreq/*cpu4-cpu*latfloor
	do
		cat $latfloor/available_frequencies | cut -d " " -f 1 > $latfloor/min_freq
		echo 8 > $latfloor/polling_interval
	done

	for latfloor in $device/*cpu7-cpu*latfloor/devfreq/*cpu7-cpu*latfloor
	do
		cat $latfloor/available_frequencies | cut -d " " -f 1 > $latfloor/min_freq
		echo 8 > $latfloor/polling_interval
	done

	for qoslat in $device/*qoslat/devfreq/*qoslat
	do
		echo 50 > $qoslat/mem_latency/ratio_ceil
	done
done

setprop vendor.post_boot.parsed 1