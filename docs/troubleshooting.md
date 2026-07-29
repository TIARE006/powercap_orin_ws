# Troubleshooting

## Invalid module format

The kernel module was built for a different kernel.

Check:

    uname -r
    modinfo runtime_monitor.ko | grep vermagic

Rebuild against the target Jetson kernel.

## I2C timeout during auto-detection

Some Jetson internal I2C adapters may not be safe to probe.

Use --skip-bus-mask.

Example: skip bus 5.

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20

## No sysfs directory

If this path does not exist:

    /sys/kernel/runtime_monitor

then the module is not loaded or failed during initialization.

Check:

    dmesg -T | tail -n 80

## Permission issue

Loading a kernel module requires root privileges. jpm uses sudo insmod internally.
