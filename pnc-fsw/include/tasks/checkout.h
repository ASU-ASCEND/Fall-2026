#pragma once

#include <Arduino.h>

// The Wi-Fi checkout AP stays up for this long after a clean boot, then
// shuts itself down. A watchdog-reset boot does not re-arm it (see
// checkout_init), so a wedge recovery does not resurrect the AP.
constexpr uint32_t CHECKOUT_DURATION_MS = 120000UL;

struct CheckoutHealth {
  bool sensors[5];
  bool sd;
  bool can;
};

void checkout_init();
void checkout_poll(const CheckoutHealth& health);
bool checkout_is_active();
