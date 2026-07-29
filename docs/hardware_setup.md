# Hardware Setup

This project uses an external INA260 sensor to measure Jetson board-level input power.

## Tested Setup

    Platform: Jetson Orin
    Kernel: 5.15.148-tegra
    Sensor: INA260
    Detected I2C bus: i2c-7
    Detected address: 0x40

## Notes

The I2C bus number may differ across Jetson models and L4T/kernel versions.

The module supports runtime auto-detection across available I2C adapters. If an internal I2C bus is unsafe to probe, use --skip-bus-mask.

Example:

    jpm monitor --duration 10 --output log.csv --skip-bus-mask 0x20
