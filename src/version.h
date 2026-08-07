#ifndef CVERTEX_VERSION_H
#define CVERTEX_VERSION_H
#include <stdio.h>
// The ONE place the build's version lives. Bump this on release; everything that shows a version —
// the boot screen (games/menu.c) and the --version flag (src/mac.c) — reads it from here, so a
// release is a one-line change and the two can never disagree.
#define CVERTEX_VERSION "0.1.0"

// 🔴 AND A SECOND, DIFFERENT QUESTION: not "which release is this" but "which BUILD am I looking
// at". They are not the same thing and conflating them helps nobody — a release number is bumped
// deliberately and rarely, while the question a screenshot raises is "is this the build you
// fixed an hour ago, or yesterday's?". On 2026-08-07 the maintainer opened cvertex.app
// expecting the current world and got one ten commits old; nothing on screen could have told him.
// So build.sh injects HEAD's date and short hash (and a trailing + for uncommitted changes) as
// CVX_VER, and it is DERIVED, never typed — a stamp someone has to remember to update is a stamp
// that lies. A plain source drop with no git still builds and honestly says "dev".
#ifndef CVX_VER
#define CVX_VER "dev"
#endif
#define CVERTEX_BUILD CVX_VER

// The window title, in one place, because three platforms each writing their own would drift — and
// drift between two copies of one derivation is the defect this codebase has met more often than
// any other. Every platform calls this; the cartridge's name comes first because that is what the
// person is actually looking at.
static inline const char *cvertex_title(const char *name) {
    static char buf[96];
    snprintf(buf, sizeof buf, "%s — v%s (%s)", name, CVERTEX_VERSION, CVERTEX_BUILD);
    return buf;
}
#endif
