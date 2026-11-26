# zettlab_dxu_fans_v2

Zettlab fan driver (hwmon) for the D6U and D8U Ultra NAS units,
running TrueNAS SCALE or any other Linux with hwmon support.

This is the v2 rewrite of the original
[Haveacry/zettlab-d8-fans](https://github.com/Haveacry/zettlab-d8-fans)
driver, superseding it under the same DKMS package name
(`zettlab-d8-fans`). The source file is `zettlab_d8_fans.c` so the
Makefile/DKMS module name matches.

## What it exposes

Three fans via `/sys/class/hwmon/hwmonX` (device name
`zettlab_d8_fans`):

| Channel | Label | RPM | PWM | Auto mode |
|---|---|---|---|---|
| fan1/pwm1 | Disks 1 | read | read/write | no |
| fan2/pwm2 | Disks 2 | read | read/write | no |
| fan3/pwm3 | CPU | read | read/write | yes |

PWM values are the raw hardware range `0..183` (0xB7). Writing `0`
to pwm3 (or setting `pwm3_enable=2`) hands the fan back to the EC
firmware — validated on D6U/D8U hardware.

## Watchdog and safe PWM

The driver includes a userspace watchdog:

- **At module load**: all fans are immediately forced to `safe_pwm`
  (default 183 = maximum), so the box is fully cooled from the moment
  the driver is loaded until the userspace controller (e.g.
  [fan-control.sh](https://github.com/mvanas/zettlab-truenas))
  takes over.
- **Periodically**: if no PWM write from userspace occurs within
  `watchdog_timeout` seconds, all fans are forced back to `safe_pwm`
  until userspace control resumes. Any userspace PWM write resets the
  watchdog.

The userspace controller must therefore write PWM values regularly
(not only on changes) to act as a heartbeat — the current
fan-control.sh does this every control cycle.

## Module parameters

| Parameter | Default | Description |
|---|---|---|
| `fan_base_addr` | `0xFE0B0456` | MMIO base address of the fan controller (read-only) |
| `watchdog_timeout` | `60` | Seconds without a userspace PWM write before safe PWM is forced; `0` disables the watchdog (runtime writable) |
| `safe_pwm` | `183` | PWM value forced at load time and by the watchdog; clamped to 183 (runtime writable) |

Example: load with a 120 s watchdog and a quieter safe value:

```
modprobe zettlab_d8_fans watchdog_timeout=120 safe_pwm=150
```

Persist options in `/etc/modprobe.d/zettlab-d8-fans.conf`:

```
options zettlab_d8_fans watchdog_timeout=120
```

## Installation (DKMS)

```
sudo mkdir -p /usr/src/zettlab-d8-fans-0.1.0
sudo cp Makefile dkms.conf zettlab_d8_fans.c /usr/src/zettlab-d8-fans-0.1.0/
sudo dkms add -m zettlab-d8-fans -v 0.1.0
sudo dkms build -m zettlab-d8-fans -v 0.1.0
sudo dkms install -m zettlab-d8-fans -v 0.1.0
sudo modprobe zettlab_d8_fans
```

When upgrading from the original 0.0.1 driver, remove the old DKMS
package first:

```
sudo dkms remove zettlab-d8-fans/0.0.1 --all
```

Verify:

```
cat /sys/class/hwmon/hwmon*/name     # expect: zettlab_d8_fans
sensors
```

## Changes vs v1 (Haveacry)

- Renamed/restructured with table-driven fan descriptors and channel
  validation
- `devm_ioremap()` + `devm_request_mem_region()` (v1 leaked the
  mapping on probe failure and module unload)
- Correct driver/device registration order
- Torn-read protection on the 16-bit RPM registers
- Watchdog + safe-PWM failsafe (new)
- Module parameters for base address, watchdog timeout and safe PWM
- Actual `.remove()` path that cancels the watchdog and re-applies
  safe PWM

## License

GPL-2.0. Original concept and first implementation by Dean Holland
(speedster@haveacry.com); v2 rewrite by Marcel van As.
