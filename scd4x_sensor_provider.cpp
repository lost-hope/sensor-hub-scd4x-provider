#include "wled.h"
#include "sensor_bus.h"
#include <SensirionI2cScd4x.h>

/*
 * SCD4x (SCD40/SCD41) true NDIR CO2 + temperature + humidity sensor
 * provider.
 *
 * Reads a Sensirion SCD4x over I2C (fixed address 0x62, no address pin)
 * and pushes three readings into the Sensor Hub (see
 * ../sensor-hub/usermod_sensor_hub.cpp and ../sensor-hub/sensor_bus.h) as
 * "<prefix>_co2", "<prefix>_temperature" and "<prefix>_humidity". Unlike
 * the BME68X provider's raw gas resistance, this is a real, calibrated CO2
 * ppm reading (NDIR optical sensing) - useful for e.g. coloring a strip by
 * air quality. This usermod never talks to MQTT, the JSON API or the Info
 * tab itself - the hub takes care of all of that once a sensor is
 * registered here.
 *
 * Wiring: SDA/SCL go to the I2C pins configured on WLED's own Config > LED
 * Preferences page (the shared "i2c_sda"/"i2c_scl" globals). WLED core
 * already calls Wire.begin() with those pins while loading cfg.json at
 * boot (wled00/cfg.cpp), before any usermod's setup() runs - so this
 * usermod only needs to confirm the pins are set, then use the shared Wire
 * bus. It must NOT call Wire.begin() itself.
 *
 * The SCD4x only produces a new reading roughly every 5s in its periodic
 * measurement mode (started once in beginSensor() and left running - it
 * must not be restarted every loop()); "Poll interval" below just controls
 * how often this usermod checks for a completed reading, not how often the
 * sensor itself measures.
 */

REGISTER_SENSOR_SLOT(_slotCo2, "_co2", SensorTypes::Co2, 0, 100);
REGISTER_SENSOR_SLOT(_slotTemp, "_temperature", SensorTypes::Temperature, 1, 100);
REGISTER_SENSOR_SLOT(_slotHumidity, "_humidity", SensorTypes::Humidity, 1, 100);

class SCD4xSensorUsermod : public Usermod {
  private:
    SensirionI2cScd4x scd4x;
    SensorHub* hub = nullptr;
    uint8_t co2Handle = SENSOR_HANDLE_INVALID;
    uint8_t tempHandle = SENSOR_HANDLE_INVALID;
    uint8_t humidityHandle = SENSOR_HANDLE_INVALID;

    bool enabled = true;
    bool sensorFound = false;
    bool initDone = false;

    unsigned long lastPoll = 0;
    unsigned long lastBeginAttempt = 0;
    uint8_t consecutiveFailures = 0;

    // config
    uint16_t pollIntervalMs = 1000; // how often to check for a completed reading (the sensor itself only produces one every ~5s)
    String namePrefix = "scd4x";    // sensor names become "<prefix>_co2/_temperature/_humidity"
    uint8_t precision = 1;          // decimal places published for temperature/humidity (co2 is always published as a whole ppm number)
    uint8_t priority = 100;         // getValue() selection priority - lower wins among sensors of the same SensorType (see sensor_bus.h)

    static const char _name[];
    static const char _enabled[];
    static const char _pollInterval[];
    static const char _namePrefix[];
    static const char _precision[];
    static const char _priority[];

    bool beginSensor() {
      scd4x.begin(Wire, 0x62);
      // Defensive: a prior boot (e.g. re-flash without a power cycle) may
      // have left the sensor mid periodic-measurement, which makes it NACK
      // startPeriodicMeasurement() below. stopPeriodicMeasurement()'s error
      // is ignored - it harmlessly fails if the sensor wasn't measuring.
      // The ~500ms wait is Sensirion's documented settle time before the
      // sensor accepts further commands.
      scd4x.stopPeriodicMeasurement();
      delay(500);
      return scd4x.startPeriodicMeasurement() == 0;
    }

    void registerSensors() {
      if (!hub || co2Handle != SENSOR_HANDLE_INVALID) return; // already registered
      co2Handle      = hub->attachSensor(&_slotCo2, namePrefix.c_str(), 0, priority);
      tempHandle     = hub->attachSensor(&_slotTemp, namePrefix.c_str(), precision, priority);
      humidityHandle = hub->attachSensor(&_slotHumidity, namePrefix.c_str(), precision, priority);
    }

    void setSensorsAvailable(bool available) {
      if (!hub) return;
      if (co2Handle != SENSOR_HANDLE_INVALID)      hub->setSensorAvailable(co2Handle, available);
      if (tempHandle != SENSOR_HANDLE_INVALID)     hub->setSensorAvailable(tempHandle, available);
      if (humidityHandle != SENSOR_HANDLE_INVALID) hub->setSensorAvailable(humidityHandle, available);
    }

  public:
    void setup() override {
      // I2C bus is configured (and Wire.begin() already called) via WLED's
      // own Config > LED Preferences page - nothing to do here if it's unset.
      // Don't persist this into 'enabled' (the user's own on/off switch) -
      // initDone (left false here) is what actually gates loop(), so a
      // later pin fix takes effect on the next boot instead of staying
      // stuck disabled.
      if (i2c_sda < 0 || i2c_scl < 0) return;
      sensorFound = beginSensor();
      initDone = true;
    }

    void loop() override {
      if (!enabled || !initDone) return;

      if (!hub) hub = getSensorHub(); // Sensor Hub usermod may finish init after us
      if (hub) registerSensors();

      unsigned long now = millis();

      if (!sensorFound) {
        // sensor missing at boot (or lost) - keep retrying rather than giving up forever
        if (now - lastBeginAttempt < 10000) return;
        lastBeginAttempt = now;
        sensorFound = beginSensor();
        if (!sensorFound) return;
      }

      if (now - lastPoll < (unsigned long)pollIntervalMs) return;
      lastPoll = now;

      bool dataReady = false;
      if (scd4x.getDataReadyStatus(dataReady) != 0 || !dataReady) return; // no new reading yet - not a failure

      uint16_t co2;
      float temperature, humidity;
      if (scd4x.readMeasurement(co2, temperature, humidity) != 0) {
        consecutiveFailures++;
        if (consecutiveFailures >= 3) setSensorsAvailable(false);
        if (consecutiveFailures >= 10) sensorFound = false; // force a fresh begin() next loop
        return;
      }

      consecutiveFailures = 0;
      setSensorsAvailable(true);
      if (hub) {
        if (co2Handle != SENSOR_HANDLE_INVALID)      hub->updateSensor(co2Handle, (float)co2);
        if (tempHandle != SENSOR_HANDLE_INVALID)     hub->updateSensor(tempHandle, temperature);
        if (humidityHandle != SENSOR_HANDLE_INVALID) hub->updateSensor(humidityHandle, humidity);
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_pollInterval)] = pollIntervalMs;
      top[FPSTR(_namePrefix)] = namePrefix;
      top[FPSTR(_precision)] = precision;
      top[FPSTR(_priority)] = priority;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_pollInterval)], pollIntervalMs);
      configComplete &= getJsonValue(top[FPSTR(_namePrefix)], namePrefix);
      configComplete &= getJsonValue(top[FPSTR(_precision)], precision);
      configComplete &= getJsonValue(top[FPSTR(_priority)], priority);
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('SCD4xSensor:pollInterval',1,'milliseconds between checks for a completed reading - the sensor itself only measures every ~5s');"));
      settingsScript.print(F("addInfo('SCD4xSensor:namePrefix',1,'sensor names become &lt;prefix&gt;_co2/_temperature/_humidity - must be unique across all sensor providers');"));
      settingsScript.print(F("addInfo('SCD4xSensor:precision',1,'decimal places published for temperature/humidity');"));
      settingsScript.print(F("addInfo('SCD4xSensor:priority',1,'getValue() selection priority - lower wins if another provider also registers a Co2/Temperature/Humidity sensor');"));
    }
};

const char SCD4xSensorUsermod::_name[]          PROGMEM = "SCD4xSensor";
const char SCD4xSensorUsermod::_enabled[]       PROGMEM = "enabled";
const char SCD4xSensorUsermod::_pollInterval[]  PROGMEM = "pollInterval";
const char SCD4xSensorUsermod::_namePrefix[]    PROGMEM = "namePrefix";
const char SCD4xSensorUsermod::_precision[]     PROGMEM = "precision";
const char SCD4xSensorUsermod::_priority[]      PROGMEM = "priority";

static SCD4xSensorUsermod scd4x_sensor;
REGISTER_USERMOD(scd4x_sensor);
