# Build and Install

## Build

From the project root:

    ./scripts/build.sh

The build script produces:

    modules/runtime_monitor/runtime_monitor.ko
    tools/jpm
    tools/jpm_aarch64

## Install

On the target Jetson:

    sudo ./scripts/install.sh

This installs:

    /lib/modules/$(uname -r)/extra/runtime_monitor.ko
    /usr/local/bin/jpm

## Run

    jpm monitor --duration 10 --output log.csv

This uses the validated defaults: 10 ms kernel period, 10 ms CSV interval, and
a 400000 Hz I2C rate request. Verify the achieved sampling rate from the CSV
timestamps.

If an unsafe I2C bus causes timeout during auto-detection:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

## Notes

The kernel module must match the target Jetson kernel. If the Jetson kernel changes, rebuild the module.
