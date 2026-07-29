# Jetson Power Monitor

A high-resolution runtime power monitoring framework for NVIDIA Jetson platforms.

This project provides a Linux kernel module and a user-space command-line tool for runtime board-level power telemetry using an external INA260 sensor.

## Features

- Kernel-side INA260 power telemetry
- Monotonic hrtimer scheduling with a high-priority workqueue for the validated 10 ms path
- Runtime INA260 auto-detection across I2C adapters
- Configurable I2C bus skip mask for unsafe/internal buses
- Sysfs telemetry interface under `/sys/kernel/runtime_monitor`
- C++ CLI tool: `jpm`
- Timestamped CSV logging
- Kernel-side sampling-limit tests for INA260 backend capacity
- Source-level build flow for Jetson kernel compatibility
- System-wide installation through `scripts/install.sh`

## Architecture

    INA260 sensor
        |
        | I2C
        v
    runtime_monitor.ko
        |
        | sysfs
        v
    /sys/kernel/runtime_monitor/*
        |
        | read by user-space CLI
        v
    jpm monitor
        |
        v
    CSV log

## Quick Start

Build the kernel module and CLI:

    ./scripts/build.sh

Install the module and CLI:

    sudo ./scripts/install.sh

Run a 10-second power logging session:

    jpm monitor --duration 10 --output log.csv

The validated default acquisition profile is 100 Hz end to end:

    kernel period:       10 ms
    user-space interval: 10 ms
    requested I2C rate:  400000 Hz

The requested interval is not proof of the achieved rate. Always calculate the
effective rate from `wall_time_sec` in the output CSV and inspect
`sample_age_ms` and `read_status`.

The periodic kernel path uses a monotonic `hrtimer` to queue reads on the
system high-priority workqueue. This is the scheduler used by the validated
100 Hz experiments; the earlier self-rescheduling `delayed_work` version is no
longer part of the project.

By default, `jpm` identifies the INA260 adapter at the platform's boot-time
rate, then requests 400 kHz before logging begins. The adapter is discovered
dynamically, so no Jetson-specific bus number or controller address is
hard-coded. Disable this behavior with
`--i2c-rate-hz 0`, or request another supported rate explicitly.

If an I2C bus causes timeout during auto-detection, skip it with a bitmask:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

## Build

From the project root:

    ./scripts/build.sh

The build script produces:

    modules/runtime_monitor/runtime_monitor.ko
    tools/jpm
    tools/jpm_aarch64

On an x86 host, `build.sh` can cross-compile the Jetson ARM64 kernel module and ARM64 `jpm` binary if the required cross-compilers and Jetson kernel source tree are available.

On a Jetson device, `build.sh` builds against the local Jetson kernel headers or kernel source.

## Install

On the target Jetson:

    sudo ./scripts/install.sh

This installs:

    /lib/modules/$(uname -r)/extra/runtime_monitor.ko
    /usr/local/bin/jpm

After installation, the command can be run from any directory:

    jpm monitor --duration 10 --output log.csv

## Main CLI Usage

Default auto-detection:

    jpm monitor --duration 10 --output log.csv

Auto-detection with unsafe I2C bus skipped:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

Manual bus/address selection:

    jpm monitor --bus-num 7 --i2c-addr 0x40 --duration 10 --output log.csv

Keep the kernel module loaded after logging:

    jpm monitor --duration 10 --output log.csv --keep-loaded

Use an already-loaded module:

    jpm monitor --duration 10 --output log.csv --no-load

## CLI Options

    --module <path>          Path to runtime_monitor.ko
    --output <file>          Output CSV path
    --duration <sec>         Logging duration in seconds
    --interval-ms <ms>       User-space logging interval (default 10 ms)
    --period-ms <ms>         Kernel sampling period (default 10 ms)
    --i2c-rate-hz <Hz>       I2C rate before logging (default 400000; 0 disables)
    --bus-num <n>            I2C bus number, -1 for auto-detect
    --i2c-addr <n>           I2C address, -1 for auto-detect, e.g., 0x40
    --skip-bus-mask <mask>   Bitmask of I2C buses to skip, e.g., 0x20
    --quiet                  Do not print each sample
    --keep-loaded            Do not unload kernel module after logging
    --no-load                Do not load/unload module; assume it is already loaded
    -h, --help               Show help

## Sysfs Interface

When the kernel module is loaded, it creates:

    /sys/kernel/runtime_monitor/

Available runtime telemetry files:

    power_mw
    voltage_mv
    current_ma
    read_status
    sample_age_ms
    period_ms
    selected_bus
    selected_i2c_addr
    auto_detect_used
    skip_bus_mask

Example:

    cat /sys/kernel/runtime_monitor/power_mw
    cat /sys/kernel/runtime_monitor/voltage_mv
    cat /sys/kernel/runtime_monitor/current_ma
    cat /sys/kernel/runtime_monitor/selected_bus
    cat /sys/kernel/runtime_monitor/selected_i2c_addr

Available kernel-side sampling-limit files:

    limit_full_run
    limit_full_samples
    limit_full_ok
    limit_full_fail
    limit_full_min_us
    limit_full_mean_us
    limit_full_p50_us
    limit_full_p90_us
    limit_full_p99_us
    limit_full_max_us
    limit_full_rate_hz

    limit_power_only_run
    limit_power_only_samples
    limit_power_only_ok
    limit_power_only_fail
    limit_power_only_min_us
    limit_power_only_mean_us
    limit_power_only_p50_us
    limit_power_only_p90_us
    limit_power_only_p99_us
    limit_power_only_max_us
    limit_power_only_rate_hz

## INA260 Auto-Detection

By default, the kernel module uses:

    bus_num = -1
    i2c_addr = -1

This means:

- Automatically scan available I2C adapters
- Scan INA260 candidate addresses from `0x40` to `0x4f`
- Validate the sensor using the INA260 manufacturer register
- Bind to the first valid INA260 device
- Expose the selected bus and address through sysfs

If a specific I2C adapter is unsafe to probe, use `--skip-bus-mask`.

Example: skip bus 5:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

because:

    0x20 = 1 << 5

## Kernel-Side Sampling-Limit Tests

The kernel module provides two sampling-limit test modes.

These tests run inside `runtime_monitor.ko` and measure the kernel-side INA260 read path directly. They do not measure user-space CSV logging overhead, sysfs read overhead, or `jpm` loop overhead.

### Full Read Limit

The full-read limit test reads all three INA260 telemetry registers:

    current + voltage + power

Run:

    sudo insmod runtime_monitor.ko bus_num=7 i2c_addr=0x40 period_ms=10

    echo 50000 | sudo tee /sys/kernel/runtime_monitor/limit_full_run

    for f in limit_full_samples limit_full_ok limit_full_fail limit_full_min_us limit_full_mean_us limit_full_p50_us limit_full_p90_us limit_full_p99_us limit_full_max_us limit_full_rate_hz; do
        printf "%-24s " "$f"
        cat /sys/kernel/runtime_monitor/$f
    done

Example result from the tested Jetson Orin setup:

    limit_full_samples       50000
    limit_full_ok            50000
    limit_full_fail          0
    limit_full_min_us        570
    limit_full_mean_us       617
    limit_full_p50_us        609
    limit_full_p90_us        703
    limit_full_p99_us        725
    limit_full_max_us        2370
    limit_full_rate_hz       1620

Interpretation:

    Full current-voltage-power read average latency: 617 us
    Estimated kernel-side backend capacity: 1.62 kHz
    Failed reads: 0 / 50000

### Power-Only Limit

The power-only limit test reads only the INA260 power register.

Run:

    sudo insmod runtime_monitor.ko bus_num=7 i2c_addr=0x40 period_ms=10

    echo 50000 | sudo tee /sys/kernel/runtime_monitor/limit_power_only_run

    for f in limit_power_only_samples limit_power_only_ok limit_power_only_fail limit_power_only_min_us limit_power_only_mean_us limit_power_only_p50_us limit_power_only_p90_us limit_power_only_p99_us limit_power_only_max_us limit_power_only_rate_hz; do
        printf "%-34s " "$f"
        cat /sys/kernel/runtime_monitor/$f
    done

Example result from the tested Jetson Orin setup:

    limit_power_only_samples           50000
    limit_power_only_ok                50000
    limit_power_only_fail              0
    limit_power_only_min_us            188
    limit_power_only_mean_us           205
    limit_power_only_p50_us            203
    limit_power_only_p90_us            210
    limit_power_only_p99_us            306
    limit_power_only_max_us            756
    limit_power_only_rate_hz           4878

Interpretation:

    Power-only read average latency: 205 us
    Estimated kernel-side backend capacity: 4.88 kHz
    Failed reads: 0 / 50000

### Limit-Test Notes

The sampling-limit tests temporarily pause periodic monitoring while the test is running. After the test finishes, periodic monitoring resumes.

The reported `limit_*_rate_hz` values are backend polling-capacity estimates:

    limit_full_rate_hz       = 1e6 / limit_full_mean_us
    limit_power_only_rate_hz = 1e6 / limit_power_only_mean_us

These values are not the same as the end-to-end CSV logging rate. End-to-end logging also includes:

- sysfs reads
- user-space scheduling
- `jpm` loop overhead
- CSV writes
- storage overhead
- system load

For stable long-running logging, use a lower sampling rate than the backend limit.

## Kernel Compatibility

Linux kernel modules are not portable across arbitrary kernel versions.

The `.ko` file must be built against the target Jetson kernel headers or kernel source. A module built for one kernel version may fail to load on another kernel version.

Check the running kernel:

    uname -r

Check module compatibility:

    modinfo runtime_monitor.ko | grep vermagic

The intended portability model is:

    Build-time compatibility:
        Build runtime_monitor.ko against the target Jetson kernel.

    Runtime compatibility:
        Auto-detect the INA260 sensor across available I2C adapters.
        Resolve the selected adapter's generic bus_clk_rate interface at runtime.

The 400 kHz setup is intentionally implemented in the user-space loader, not
inside `runtime_monitor.ko`. If a Jetson kernel does not expose
`/sys/class/i2c-adapter/i2c-N/bus_clk_rate`, `jpm` prints a warning and keeps
that platform's default rate. This preserves support for Jetson variants with
different I2C controller paths and bus numbering.

## Hardware Setup

This project uses an external INA260 sensor to measure Jetson board-level input power.

Current tested setup:

    Platform: Jetson Orin
    Kernel: 5.15.148-tegra
    Sensor: INA260
    Detected I2C bus: i2c-7
    Detected address: 0x40

The I2C bus number may differ across Jetson models, JetPack/L4T versions, and device-tree configurations. The module is designed to avoid relying on a fixed I2C bus number.

## Example Output

    [jpm] loading module: /home/yuanyang/runtime_monitor.ko
    [jpm] bus_num=-1 i2c_addr=-1 period_ms=10 skip_bus_mask=0x20
    [jpm] sysfs: /sys/kernel/runtime_monitor
    [jpm] selected_bus=7
    [jpm] selected_i2c_addr=0x40
    [jpm] skip_bus_mask=0x20
    [jpm] logging to log.csv
    [jpm] interval_ms=10 duration_sec=10
    t=   0.000s power= 6680 mW voltage= 19243 mV current= 347 mA status=0 age=4 ms
    [jpm] logging finished
    [jpm] unloading runtime_monitor

## CSV Format

The output CSV contains:

    wall_time_sec,elapsed_ms,power_mw,voltage_mv,current_ma,read_status,sample_age_ms

Column meanings:

    wall_time_sec    Wall-clock timestamp in seconds
    elapsed_ms       Time elapsed since logging started
    power_mw         Board-level input power in milliwatts
    voltage_mv       Input voltage in millivolts
    current_ma       Input current in milliamps
    read_status      0 means successful sensor read
    sample_age_ms    Age of the latest kernel sample when read by user space

## Troubleshooting

### Invalid module format

The module was built for a different kernel.

Check:

    uname -r
    modinfo runtime_monitor.ko | grep vermagic

Rebuild against the target Jetson kernel.

### I2C timeout during auto-detection

Some Jetson internal I2C adapters may not be safe to probe.

Use `--skip-bus-mask`.

Example:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

### No sysfs directory

If this path does not exist:

    /sys/kernel/runtime_monitor

then the module is not loaded or failed during initialization.

Check:

    dmesg -T | tail -n 80

### Permission issue

Loading a kernel module requires root privileges. `jpm` uses `sudo insmod` internally.

## Project Status

V1.0-alpha includes:

- `runtime_monitor.ko`
- `jpm` CLI
- `scripts/build.sh`
- `scripts/install.sh`
- INA260 auto-detection
- configurable `skip_bus_mask`
- sysfs telemetry
- CSV logging
- kernel-side full-read sampling-limit test
- kernel-side power-only sampling-limit test

`jpm monitor` is the only supported logging entry point. The obsolete
`tools/run_monitor.sh` wrapper was removed because it depended on a missing
legacy `runtime_logger.py` and duplicated functionality already implemented by
`jpm`.

## Repository Notes

Do not commit generated kernel artifacts, binaries, logs, or NVIDIA L4T kernel source trees.

Ignored examples include:

    *.ko
    *.o
    *.mod
    Module.symvers
    modules.order
    tools/jpm
    tools/jpm_aarch64
    *.csv
    l4t/
    Linux_for_Tegra/
    notes/
