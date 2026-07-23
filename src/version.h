#ifndef CVERTEX_VERSION_H
#define CVERTEX_VERSION_H
// The ONE place the build's version lives. Bump this on release; everything that shows a version —
// the boot screen (games/menu.c) and the --version flag (src/mac.c) — reads it from here, so a
// release is a one-line change and the two can never disagree.
#define CVERTEX_VERSION "0.1.0"
#endif
