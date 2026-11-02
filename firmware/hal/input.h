// The board's inputs: the sensor's switch (a contact sensor's reed switch, a motion sensor's PIR) and its
// button. Levels are logical (true: open / motion / pressed), whatever the pins' polarity. A change wakes
// clock_wait(), also from standby; the app debounces.
#pragma once

#include <stdbool.h>

enum { INPUT_EVENT, INPUT_BUTTON, INPUT_COUNT };

#define INPUT_PULL_DEFAULT (-1)

// Whether the board has the input.
bool input_present(int input);

bool input_level(int input);

// Overrides INPUT_EVENT polarity (invert flips the board's default active level) and internal resistor pull
// (BOARD_PULL_UP / BOARD_PULL_NONE / BOARD_PULL_DOWN, or INPUT_PULL_DEFAULT for the board's default).
void input_configure_event(bool invert, int pull);
