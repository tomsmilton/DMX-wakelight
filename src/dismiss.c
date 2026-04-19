#include "dismiss.h"

#include <stdatomic.h>
#include <time.h>

// Encodes year/month/day in one int so we can compare atomically.
// Sentinel 0 = not dismissed.
static _Atomic int g_dismissed_ord = 0;

static int ord_today(void) {
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  return (lt.tm_year + 1900) * 512 + (lt.tm_mon + 1) * 32 + lt.tm_mday;
}

void dismiss_for_today(void) { atomic_store(&g_dismissed_ord, ord_today()); }
void dismiss_clear(void) { atomic_store(&g_dismissed_ord, 0); }

bool dismiss_is_active(void) {
  int d = atomic_load(&g_dismissed_ord);
  if (d == 0) return false;
  return d == ord_today();
}
