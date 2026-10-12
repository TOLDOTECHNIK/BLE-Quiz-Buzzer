#include "nrf.h"
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>

Adafruit_USBD_HID usb_hid;

BLEDis bledis;
BLEBas blebas;
BLEHidAdafruit blehid;

#define DEVICE_MANUFACTURER "TOLDO TECHNIK"
#define DEVICE_NAME "BLE Quiz Buzzer"
#define DEVICE_VERSION "1.1.0"

#define PIN_BUTTON PIN_006
#define PIN_LED_RED PIN_015
#define PIN_POWER PIN_013

#define BATTERY_UPDATE_MS 2000
#define VDDHDIV 5

#define PRESS_COOLDOWN_MS 1000
#define SLEEP_TIMEOUT_MS 10 * 60000 // 10 mins

uint32_t lastActivity  = 0;
uint32_t lastPressTime = 0;
uint32_t lastBlinkTime = 0;

uint32_t lastBatteryUpdate = 0;

bool buttonPressed = false;
bool ledState      = false;

uint8_t const ble_hid_report_descriptor[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

void startAdv() {
  Bluefruit.Advertising.stop();

  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

  Bluefruit.Advertising.addTxPower();

  // Important: makes host show keyboard instead of mouse/generic HID
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);

  Bluefruit.Advertising.addName();
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.addService(blehid);
  Bluefruit.Advertising.addService(blebas);

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
}

void sendUsbKey(uint8_t keycode, uint8_t mod = 0) {
  uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};

  usb_hid.keyboardReport(0, mod, keys);
  delay(5);

  usb_hid.keyboardRelease(0);
  delay(5);
}

void sendBleKey(uint8_t keycode, uint8_t mod = 0) {
  uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};

  blehid.keyboardReport(0, mod, keys);
  delay(5);

  blehid.keyRelease();
  delay(5);
}

void sendKey(uint8_t keycode, uint8_t mod = 0) {
  // Prefer USB when plugged in, otherwise use BLE.
  // This prevents sending the same key to two devices at once.
  if (USBDevice.mounted()) {
    sendUsbKey(keycode, mod);
  } else if (Bluefruit.connected()) {
    sendBleKey(keycode, mod);
  }

  lastActivity = millis();
}

void blinkLed(uint16_t ms = 10) {
  digitalWrite(PIN_LED_RED, HIGH);
  delay(ms);
  digitalWrite(PIN_LED_RED, LOW);
}

void updateStatusLed() {
  // LED logic based on your current working code:
  // LOW  = off / normal
  // HIGH = on

  if (Bluefruit.connected()) {
    digitalWrite(PIN_LED_RED, LOW);    // OFF when BLE connected
    return;
  }

  if (USBDevice.mounted()) {
    digitalWrite(PIN_LED_RED, HIGH);    // solid ON when USB mounted
    return;
  }

  // slow blink while waiting / advertising
  uint32_t now = millis();

  if (now - lastBlinkTime > 500) {
    lastBlinkTime = now;
    ledState      = !ledState;
    digitalWrite(PIN_LED_RED, ledState ? HIGH : LOW);
  }
}

static uint8_t lithiumIonMvToPct(int32_t mv) {
  // Linear estimation curve:
  // 3.3V = 0%, 3.7V-4.2V = 100%
  if (mv >= 3700)
    return 100;
  if (mv <= 3300)
    return 0;
  return (uint8_t)((mv - 3300) / 4);
}

int32_t readVddhMv() {
  static float filteredBatteryMv = 0.0f;
  static bool initialized = false;

  constexpr float alpha = 0.15f;

  analogReadResolution(10);
  analogReference(AR_INTERNAL);
  analogSampleTime(40);

#ifdef SAADC_CH_PSELP_PSELP_VDDHDIV5
  uint32_t raw = analogReadVDDHDIV5();

  int32_t measuredMv = (raw * 3600L) / 1023L;
  int32_t batteryMv  = measuredMv * 5L;
#else
  uint32_t raw = analogReadVDD();

  int32_t batteryMv = (raw * 3600L) / 1023L;
#endif

  if (!initialized) {
    filteredBatteryMv = batteryMv;
    initialized = true;
  } else {
    filteredBatteryMv =
        alpha * batteryMv +
        (1.0f - alpha) * filteredBatteryMv;
  }

  int32_t filteredMv = (int32_t)(filteredBatteryMv + 0.5f);

  Serial.print("raw=");
  Serial.print(batteryMv);
  Serial.print("mV filtered=");
  Serial.print(filteredMv);
  Serial.println("mV");

  return filteredMv;
}

uint8_t readBatteryPercent() {
  int32_t batteryMv = readVddhMv();
  uint8_t pct = lithiumIonMvToPct(batteryMv);

  Serial.print("Battery report: ");
  Serial.print(batteryMv);
  Serial.print(" mV => ");
  Serial.print(pct);
  Serial.println("%");

  return pct;
}

void lowPowerPin(uint32_t arduinoPin) {
  nrf_gpio_cfg(g_ADigitalPinMap[arduinoPin], NRF_GPIO_PIN_DIR_INPUT,
               NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
}

void goToSleep() {
  usb_hid.keyboardRelease(0);
  blehid.keyRelease();

  USBDevice.detach();

  Bluefruit.Advertising.stop();

  if (Bluefruit.connected()) {
    Bluefruit.disconnect(Bluefruit.connHandle());
    delay(100);
  }

  digitalWrite(PIN_LED_RED, LOW);
  delay(20);

  lowPowerPin(PIN_LED_RED);

  nrf_gpio_cfg_sense_input(g_ADigitalPinMap[PIN_BUTTON], NRF_GPIO_PIN_PULLUP,
                           NRF_GPIO_PIN_SENSE_LOW);

  delay(10);

  sd_power_system_off();

  while (1) {
    __WFE();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  pinMode(PIN_LED_RED, OUTPUT);
  digitalWrite(PIN_LED_RED, LOW);

  pinMode(PIN_POWER, OUTPUT);     // disable external power LDO
  digitalWrite(PIN_POWER, LOW);   // saves much current: ON = 49.70uA, OFF = 9.10uA (measured with
                                  // PPK2) To be sure, diode RB551V-30 soldered on (see:
                                  // https://github.com/sasodoma/nrf52840-promicro#sleep-current-problems)

  USBDevice.setID(0xFED0, 0x2468);
  USBDevice.setManufacturerDescriptor(DEVICE_MANUFACTURER);
  USBDevice.setProductDescriptor(DEVICE_NAME);
  USBDevice.setSerialDescriptor(DEVICE_VERSION);

  static uint8_t const usb_hid_report_descriptor[] = {
      TUD_HID_REPORT_DESC_KEYBOARD()};

  usb_hid.setReportDescriptor(usb_hid_report_descriptor,
                              sizeof(usb_hid_report_descriptor));

  usb_hid.begin();

  Bluefruit.begin();
  Bluefruit.setName(DEVICE_NAME);

  blehid.setReportMap(ble_hid_report_descriptor,
                      sizeof(ble_hid_report_descriptor));

  blehid.begin();

  blebas.begin();
  blebas.write(readBatteryPercent());

  bledis.setManufacturer(DEVICE_MANUFACTURER);
  bledis.setModel(DEVICE_NAME);
  bledis.setFirmwareRev(DEVICE_VERSION);
  bledis.begin();

  startAdv();

  lastActivity = millis();
}

void loop() {
  updateStatusLed();

  bool state = digitalRead(PIN_BUTTON) == LOW;

  if (state && !buttonPressed) {
    uint32_t now = millis();

    if ((now - lastPressTime) > PRESS_COOLDOWN_MS) {
      lastPressTime = now;
      buttonPressed = true;

      sendKey(HID_KEY_A);
      sendKey(HID_KEY_SPACE);

      blinkLed(10);
    }
  }

  if (!state && buttonPressed) {
    buttonPressed = false;
    lastActivity  = millis();
  }

  if ((millis() - lastBatteryUpdate) > BATTERY_UPDATE_MS) {
    lastBatteryUpdate = millis();

    uint8_t batt = readBatteryPercent();
    blebas.write(batt);

    if (Bluefruit.connected()) {
      blebas.notify(batt);
    }
  }

  if ((millis() - lastActivity) > SLEEP_TIMEOUT_MS) {
    goToSleep();
  }
  delay(5);
}