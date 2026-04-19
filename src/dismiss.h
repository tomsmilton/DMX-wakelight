#pragma once

#include <stdbool.h>

// "Done for today" — suppresses the AUTO schedule output until local midnight
// rolls the calendar date. Ephemeral: not persisted across reboot.
void dismiss_for_today(void);

// Clears any pending dismiss (i.e. "undo").
void dismiss_clear(void);

// True if the current local date matches the date the user dismissed.
bool dismiss_is_active(void);
