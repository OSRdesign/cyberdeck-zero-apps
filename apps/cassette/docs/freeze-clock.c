/* LD_PRELOAD shim for make-doc-images.sh: the wall clock stands still at 2026-11-11 12:00:00 UTC, so the header
 * clock of the rendered pictures does not change from run to run. Monotonic clocks are untouched. */
#include <time.h>

time_t time(time_t *t)
{
    const time_t fixed = 1794398400;
    if (t) *t = fixed;
    return fixed;
}
