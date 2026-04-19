#pragma once

#include <stdbool.h>

// "Done for today" — terminal for the current local date. Suppresses the
// AUTO schedule output until midnight rolls. Persisted to NVS so a reboot
// before midnight still honours the dismissal. No undo.
void dismiss_init(void);         // load stored date from NVS at boot
void dismiss_for_today(void);    // dismiss now + persist
void dismiss_reset(void);        // clear stored dismiss (e.g. when a new
                                 // future schedule is saved). Not exposed
                                 // via UI — only triggered programmatically.
bool dismiss_is_active(void);    // true while today's date matches stored date
