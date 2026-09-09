# SCD4x CO2 Sensor Provider

A [Sensor Hub](../sensor-hub/readme.md) provider usermod for the Sensirion
SCD40/SCD41 - a true NDIR CO2 sensor (plus integrated temperature/humidity)
at a fixed I2C address (`0x62`, no address pin, so only one per I2C bus).
It registers three sensors with the hub - `scd4x_co2`, `scd4x_temperature`
and `scd4x_humidity` by default - and lets the hub handle MQTT, Home
Assistant discovery, the JSON API and the Info tab.

Unlike [`sensor-hub-bme68x-provider`](../sensor-hub-bme68x-provider/readme.md)'s
raw, uncalibrated gas resistance, this is a real, calibrated CO2 ppm
reading - useful for e.g. coloring a strip by air quality.

See [`scd4x_sensor_provider.cpp`](scd4x_sensor_provider.cpp) for the full
source and the [Sensor Hub readme](../sensor-hub/readme.md) for how the
bus/hub work.

## Hardware

Wire the SCD4x to the I2C pins configured on WLED's own
**Config > LED Preferences** page (SDA/SCL, shared across all I2C usermods).
WLED itself calls `Wire.begin()` with those pins at boot, before any
usermod's `setup()` runs - this usermod only checks the pins are set and
then uses the already-initialized bus.

The sensor runs in its own periodic measurement mode and only produces a
new reading roughly every 5 seconds regardless of how often it's polled -
**Poll interval** just controls how often this usermod checks for a
completed reading, not how fast the sensor itself measures.

If the sensor isn't found at boot (not wired up yet, still powering up,
etc.) it keeps retrying every 10s rather than giving up.

## Usage

Add `sensor-hub-scd4x-provider` to `custom_usermods` next to the
[Sensor Hub](../sensor-hub/readme.md) itself.

## Usermod Settings

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch (also auto-disabled if I2C pins aren't configured) |
| Poll interval | 1000 ms | How often to check for a completed reading |
| Name prefix | `scd4x` | Sensor names become `<prefix>_co2` / `_temperature` / `_humidity` - must be unique across every provider registered with the hub |
| Precision | 1 | Decimal places published for temperature/humidity (CO2 is always a whole ppm number) |
| Priority | 100 | `getValue()` selection priority - lower wins if another provider also registers a Co2/Temperature/Humidity sensor |
