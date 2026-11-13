#include "tasks/watchdog.h"

#include "SysHead.h"
#include "hardware/watchdog.h"
#include "pico/mutex.h"

#define WATCHDOG_INTERVAL_MS 8000
// Bench A/B: the RP2350 WDT is firing on a starved feed and the external ATtiny
// is not programmed yet. Disable it to see whether anything else resets the
// board; restore to 1 once the feed/stall is fixed.
#define PNC_WATCHDOG_ENABLE 1

#define WATCHDOG_CONNECTED_TASKS 2
#define WATCHDOG_INTERTASK_CHECK_PERIOD_MS 1000 * 60  // 1 minute

#if WATCHDOG_CONNECTED_TASKS > 0
static bool heartbeats[WATCHDOG_CONNECTED_TASKS];
#endif
static mutex_t heartbeats_mutex;

static ErrorDisplay& error_display = ErrorDisplay::instance();

static uint32_t last_check = 0;
static volatile bool watchdog_paused = false;

void watchdog_pause() {
  watchdog_paused = true;
  watchdog_disable();
}

void watchdog_resume() {
  watchdog_paused = false;
  if (PNC_WATCHDOG_ENABLE) {
    watchdog_enable(WATCHDOG_INTERVAL_MS, true);
    watchdog_update();
  }
}

void watchdog_task() {
  // A scoped pause (RadiaCode BLE) owns the watchdog; do not re-enable it from
  // this task while the blocking call is in progress.
  if (PNC_WATCHDOG_ENABLE && !watchdog_paused) {
    watchdog_enable(WATCHDOG_INTERVAL_MS, true);
    // Kick the hardware watchdog before any logging: Serial can block behind a
    // long BLE/SD stall on core 0, and an unfed 5 s watchdog resets the chip.
    watchdog_update();
  }

  // Binary packet mode needs a clean serial stream.
  if (!pnc_binary_mode) {
    log_task("Watchdog it");
  }
  error_display.toggle();

  // Also feed the external ATtiny watchdog from core 1 so a slow SD write or a
  // cold-boot sensor retry on core 0 does not reset the Pico.
  static bool ext_heartbeat = false;
  ext_heartbeat = !ext_heartbeat;
  digitalWrite(WATCHDOG_HEARTBEAT_PIN, ext_heartbeat);

  watchdog_update();

  // Do not run the 60 s intertask check while a scoped pause has the hardware
  // watchdog disabled; a freeze here would be unrecoverable.
  if (!PNC_WATCHDOG_ENABLE || watchdog_paused) return;

  // check intertask
  mutex_enter_blocking(&heartbeats_mutex);
  if (millis() - last_check > WATCHDOG_INTERTASK_CHECK_PERIOD_MS) {
    // Stamp the check: without this the block re-arms on every call (1 Hz) and
    // can catch a heartbeat between two updates, freezing the board.
    last_check = millis();
    // check heartbeats
    for (size_t i = 0; i < WATCHDOG_CONNECTED_TASKS; i++) {
      // if a heartbeat hasn't toggled since last check
      if (heartbeats[i] == false) {
        // watchdog freeze
        log_task_printf(
            "Watchdog freeze: task %u missing (hb0=%d hb1=%d)\n",
            (unsigned)i, (int)heartbeats[0], (int)heartbeats[1]);
        while (1);
      }
      // reset heartbeat
      heartbeats[i] = false;
    }
  }
  mutex_exit(&heartbeats_mutex);
}

static bool watchdog_intertask_update_disabled = true;
void watchdog_intertask_update(uint8_t id) {
  if (watchdog_intertask_update_disabled) return;
  if (mutex_try_enter_block_until(&heartbeats_mutex,
                                  make_timeout_time_ms(100))) {
    heartbeats[id] = true;
    mutex_exit(&heartbeats_mutex);
  }
}

void watchdog_task_init() {
  // setup task
  mutex_init(&heartbeats_mutex);
  watchdog_intertask_update_disabled = false;  // toggle

  log_task("Watchdog task started.");
}