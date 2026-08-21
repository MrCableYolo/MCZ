// display_none.cpp — headless build: no display hardware, no-op implementation.
#include "display.h"

namespace {
class NoDisplay : public DisplayUI {
  // inherits the empty default methods; renders nothing, provides no inputs.
};
NoDisplay g_noDisplay;
}

DisplayUI& displayInstance(){ return g_noDisplay; }
