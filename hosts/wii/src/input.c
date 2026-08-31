/* Wii Remote input mapped to contracts/spec/spec.ts BTN. */

#include "input.h"
#include "pocket_spec.h"

#include <wiiuse/wpad.h>

void wii_input_scan(void) {
  WPAD_ScanPads();
}

int32_t wii_input_buttons(void) {
  uint32_t held = WPAD_ButtonsHeld(WPAD_CHAN_0);

  /* HOME belongs to the Wii shell. Strip both Wii Remote and Classic
   * Controller encodings before any device bits reach globalThis.frame. */
  held &= ~WPAD_BUTTON_HOME;
  held &= ~WPAD_CLASSIC_BUTTON_HOME;

  int32_t buttons = 0;
  if (held & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP)) buttons |= POCKET_BTN_UP;
  if (held & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) buttons |= POCKET_BTN_RIGHT;
  if (held & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN)) buttons |= POCKET_BTN_DOWN;
  if (held & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) buttons |= POCKET_BTN_LEFT;
  if (held & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) buttons |= POCKET_BTN_CIRCLE;
  if (held & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) buttons |= POCKET_BTN_CROSS;
  if (held & (WPAD_BUTTON_1 | WPAD_CLASSIC_BUTTON_Y)) buttons |= POCKET_BTN_TRIANGLE;
  if (held & (WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_X)) buttons |= POCKET_BTN_SQUARE;
  if (held & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) buttons |= POCKET_BTN_START;
  if (held & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) buttons |= POCKET_BTN_SELECT;
  return buttons;
}

bool wii_input_home_down(void) {
  const uint32_t down = WPAD_ButtonsDown(WPAD_CHAN_0);
  return (down & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) != 0;
}
