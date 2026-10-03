#include "PowerManager.h"

#include <Arduino.h>
#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_EINK4
#include <MetalioEink4Board.h>
#endif
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc_caps.h>
#if FREEINK_DEVICE_WS397
#include <Axp2101.h>
#endif

namespace freeink {
namespace {
int8_t powerPin() { return BoardConfig::ACTIVE.input.power; }
bool powerActiveHigh() { return BoardConfig::ACTIVE.input.powerActiveHigh; }
PowerManager::HostShutdownHook g_hostShutdownHook = nullptr;
// Records a deepSleep() abort (see deepSleep()) across the esp_restart() that
// follows it, so the next boot can report it via takeAbortedSleepInfo().
// RTC_NOINIT_ATTR survives esp_restart() (a warm reset, same RTC-memory domain)
// but is garbage on a cold boot -- magic-guarded like main.cpp's silentRebootMagic.
constexpr uint32_t ABORTED_SLEEP_MAGIC = 0x41424f52;  // 'ABOR'

struct AbortedSleepRecord {
  bool aborted;
  int wakeupCause;
  int wakePinLevel;
};
}  // namespace

void PowerManager::setHostShutdownHook(const HostShutdownHook hook) { g_hostShutdownHook = hook; }

bool PowerManager::isDeepSleepWakePin(const int8_t pin) {
  if (pin < 0) return false;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // Xtensa (S3/S2, classic ESP32): ext1 can only arm an RTC-capable pad, so a
  // digital-only line (Read Pico FCA9555 INT# = GPIO41, CST836U INT# = GPIO43)
  // is out by construction, not by policy.
  return rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin));
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // RISC-V (C3/C6/H2): the deep-sleep "gpio" source takes ordinary digital pins.
  return static_cast<gpio_num_t>(pin) < GPIO_NUM_MAX;
#else
  return false;
#endif
}

bool PowerManager::armLightSleepWakeupLevels(const uint64_t lowMask, const uint64_t highMask) {
  bool armed = false;
  for (uint8_t pin = 0; pin < static_cast<uint8_t>(GPIO_NUM_MAX); ++pin) {
    const uint64_t bit = 1ULL << pin;
    if ((lowMask & bit) == 0 && (highMask & bit) == 0) continue;
    const esp_err_t err = gpio_wakeup_enable(static_cast<gpio_num_t>(pin),
                                             (lowMask & bit) != 0 ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (err != ESP_OK) {
      clearLightSleepWakeup(lowMask | highMask);
      return false;
    }
    armed = true;
  }
  if (!armed) return false;
  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) {
    clearLightSleepWakeup(lowMask | highMask);
    return false;
  }
  return true;
}

bool PowerManager::armLightSleepWakeup(const uint64_t gpioMask, const bool wakeLow) {
  return armLightSleepWakeupLevels(wakeLow ? gpioMask : 0ULL, wakeLow ? 0ULL : gpioMask);
}

void PowerManager::clearLightSleepWakeup(const uint64_t gpioMask) {
  for (uint8_t pin = 0; pin < static_cast<uint8_t>(GPIO_NUM_MAX); ++pin) {
    if ((gpioMask & (1ULL << pin)) == 0) continue;
    (void)gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
  }
  // The level triggers above are per pin, but the wakeup SOURCE is shared: it has
  // to be disabled as well, or the next light sleep would still be armed.
  (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

RTC_NOINIT_ATTR uint32_t abortedSleepMagic;
RTC_NOINIT_ATTR AbortedSleepRecord abortedSleepRecord;

void PowerManager::armWakeOnPins(uint64_t gpioMask, bool wakeLow) {
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // Xtensa (S3/S2, classic ESP32): RTC ext1. Pins must be RTC GPIOs.
  //
  // The classic ESP32 RTC has no "any low" mode — only ESP_EXT1_WAKEUP_ALL_LOW
  // ("wake when ALL selected pins are low"). For a single wake pin (the common
  // power-button case) ALL_LOW and ANY_LOW are identical; a multi-pin low wake on
  // classic ESP32 fires only when every pin is low. S2/S3 expose ANY_LOW directly.
#if defined(CONFIG_IDF_TARGET_ESP32)
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ALL_LOW;
#else
  const esp_sleep_ext1_wakeup_mode_t lowMode = ESP_EXT1_WAKEUP_ANY_LOW;
#endif
  esp_sleep_enable_ext1_wakeup(gpioMask, wakeLow ? lowMode : ESP_EXT1_WAKEUP_ANY_HIGH);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
  // RISC-V (C3/C6/H2): the deep-sleep "gpio" wakeup source.
  esp_deep_sleep_enable_gpio_wakeup(gpioMask, wakeLow ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error "FreeInk PowerManager: target has no supported deep-sleep GPIO wakeup source"
#endif
}

bool PowerManager::armPowerButtonWakeup() {
  const int8_t pin = powerPin();
  if (pin < 0) return false;
  const bool activeHigh = powerActiveHigh();

  // Hold the idle level with the opposite pull so the line is defined in sleep.
  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  armWakeOnPins(1ULL << pin, /*wakeLow=*/!activeHigh);
  return true;
}

void PowerManager::waitForPowerButtonRelease() {
  const int8_t pin = powerPin();
  if (pin < 0) return;
  const bool activeHigh = powerActiveHigh();

  pinMode(pin, activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  const int pressedLevel = activeHigh ? HIGH : LOW;
  while (digitalRead(pin) == pressedLevel) {
    delay(50);
  }
}

namespace {
// Drive a rail pin to `level` and latch it so the level survives deep
// sleep (requires gpio_deep_sleep_hold_en(), done in deepSleep()). gpio_hold_dis
// first: a hold left over from a previous cycle would make the writes no-ops.
void holdRailLevel(int8_t pin, uint8_t level) {
  if (pin < 0) return;
  const auto g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
  gpio_hold_en(g);
}
}  // namespace

void PowerManager::powerDownRailsForSleep() {
  const auto& b = BoardConfig::ACTIVE;
#if FREEINK_DEVICE_METALIO_EINK4
  if (metalio::ready && !metalio::setOutput(metalio::output & ~metalio::PA_POWER)) {
    if (Serial) Serial.printf("[PM] Metalio amplifier shutdown failed\n");
  }
  digitalWrite(44, LOW);
  // Keep the shared display/SD rail up, as required by the calibrated BSP.
#endif
#if FREEINK_DEVICE_WS397
  // The EPD rail is an AXP2101 LDO, not a GPIO, so holdRailOff() below cannot
  // reach it — drop it here or the panel stays powered all through deep sleep.
  axp2101::setEpdPower(false);
#endif
  // Keep RESET defined through deep sleep, but never drive an unpowered panel's
  // input HIGH: on boards with a gated EPD rail (Sticky), that can back-power the
  // controller through its RESET protection diode and turn sleep into a
  // milliamp-level drain. Hold RESET LOW alongside a switched-off rail. Boards
  // whose panel rail remains powered (X4 Pro) keep RESET HIGH so a UC8179 cannot
  // drift out of DSLP and restart its analog booster. EpdBus and XteinkDetect
  // release the hold before issuing a reset pulse on wake.
  const uint8_t resetSleepLevel = b.display.powerEnable >= 0 ? LOW : HIGH;
  holdRailLevel(b.display.rst, resetSleepLevel);
#if FREEINK_DEVICE_EEGO_A4
  // EEGO's GPIO4 battery latch must stay asserted for GPIO8 to wake the S3
  // from real deep sleep instead of forcing the next press through a cold boot.
  if (BoardConfig::isEegoA4()) holdRailLevel(b.power.latch0, HIGH);
#endif
  holdRailLevel(b.display.powerEnable, LOW);
  // SD enable OFF = the inactive level: LOW for active-high enables, HIGH for the
  // active-low ones (e.g. X4 Pro's GPIO5, which powers the card while held LOW).
  holdRailLevel(b.sd.powerEnable, b.sd.powerActiveHigh ? LOW : HIGH);
  holdRailLevel(b.touch.powerEnable, b.touch.powerEnableActiveHigh ? LOW : HIGH);
  // The mic enable also carries a polarity flag; OFF is the inactive level.
  holdRailLevel(b.mic.enable, b.mic.enableActiveHigh ? LOW : HIGH);
}

void PowerManager::deepSleep() {
  if (g_hostShutdownHook != nullptr) {
    // The board supports no ESP-side deep-sleep wake source (its "off" is a
    // PMU-driven host shutdown), so esp_deep_sleep_start() would strand the chip.
    // Hand the shutdown to the board; a return means the rail was never cut, and
    // there is nothing left that could wake this board, so idle rather than sleep.
    g_hostShutdownHook();
    while (true) {
      delay(1000);
    }
  }
  esp_sleep_config_gpio_isolate();
#if !FREEINK_MCU_C61
  gpio_deep_sleep_hold_en();
#endif
  esp_deep_sleep_start();
  // esp_deep_sleep_start() does not return in normal operation -- reaching
  // this line means sleep entry was rejected/aborted by the SoC rather than
  // the next boot being a fresh wake. A busy-loop here draws full-clock
  // current (~20-30mA) indefinitely and never responds to the power button,
  // making an aborted sleep entry indistinguishable from a real drain bug.
  // Record the abort (RTC_NOINIT_ATTR survives the reset below) so the next
  // boot can report it via takeAbortedSleepInfo(), then reset for real via
  // esp_restart() -- itself [[noreturn]], satisfying this function's contract.
  const int8_t pin = powerPin();
  abortedSleepRecord.aborted = true;
  abortedSleepRecord.wakeupCause = static_cast<int>(esp_sleep_get_wakeup_cause());
  abortedSleepRecord.wakePinLevel = pin >= 0 ? digitalRead(pin) : -1;
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  esp_restart();
}

void PowerManager::deepSleepUntilPowerButton() {
  waitForPowerButtonRelease();
  armPowerButtonWakeup();
  deepSleep();
}

PowerManager::AbortedSleepInfo PowerManager::takeAbortedSleepInfo() {
  AbortedSleepInfo info;
  if (abortedSleepMagic == ABORTED_SLEEP_MAGIC && abortedSleepRecord.aborted) {
    info.aborted = true;
    info.wakeupCause = abortedSleepRecord.wakeupCause;
    info.wakePinLevel = abortedSleepRecord.wakePinLevel;
  }
  // Clear so a stale record doesn't get reported again on a later boot, and
  // re-stamp the magic so an uninitialized cold-boot read still reports false
  // rather than following a garbage `aborted` bit.
  abortedSleepRecord.aborted = false;
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  return info;
}

}  // namespace freeink
