// display.h — Display abstraction (hardware-agnostic). Only display_none.cpp
// implements it in this headless build; kept as an interface for future use.
#pragma once
#include "oven.h"

// Control intent that a display (buttons/touch/encoder) reports back to main.
struct InputEvent {
  enum Type { NONE, SET_TEMP, SET_POWER, SET_MODE, ON, OFF } type = NONE;
  float fval = 0;   // for SET_TEMP (degrees C)
  int   ival = 0;   // for SET_POWER (1..5) / SET_MODE (0..4)
};

class DisplayUI {
public:
  virtual ~DisplayUI() {}
  virtual void begin() {}                         // once in setup()
  virtual void tick() {}                           // EVERY loop (e.g. lv_timer_handler)
  virtual void render(const OvenState& s) {}      // periodically with current state
  virtual InputEvent poll() { return InputEvent(); } // query inputs (non-blocking)
};

// Returns the active implementation. Exactly one .cpp defines this
// (display_none.cpp as default; a HW driver replaces it later).
DisplayUI& displayInstance();
