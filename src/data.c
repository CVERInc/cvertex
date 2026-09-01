// data.c — where the console keeps its own files.
//
// A console that remembers anything has to answer one question first: WHERE. The obvious
// answer — "next to the executable" — is right most of the time and catastrophically wrong
// the rest of it: macOS App Translocation runs a downloaded .app from a read-only shadow
// mount, /Applications is not writable by a non-admin user, and a build handed round on a
// USB stick or a CD is read-only by construction. Every one of those looks identical from
// inside the program: fopen(...,"w") returns NULL and the feature quietly stops working.
//
// So the primary is still the executable's own directory (a portable install stays portable —
// copy the folder, keep your shelf order), and the fallback is the per-user data directory
// the platform already has a convention for. Which one we got is not a secret: cvx_data_dir()
// hands it back so a debug overlay can print it, because "it isn't saving" is a question
// nobody can answer without knowing where it tried.
//
// 🔴 RESOLUTION CREATES NOTHING. Choosing the directory is access()/readlink only — no probe
// file, no mkdir — so --headless / --ppm / --dump leave the filesystem exactly as they found
// it. Directories are created only on cvx_data_path(file, 1), i.e. only when a caller has
// actually said it is about to write. That is why the for_write argument exists: the contract
// in core.h is "asking where costs nothing; saying you will write is what makes a directory".
//
// One file, fenced with #ifdef, rather than three copies in mac.c/lnx.c/win.c — the same
// bargain net.c already makes. Three copies of one derivation is the defect this codebase
// has met more often than any other.
#include "core.h"
#include "version.h"   // CVERTEX_NAME — the product name lives in exactly one place
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #define PSEP      '\\'
  #define MKDIR(p)  _mkdir(p)
  #define WRITABLE(p) (_access((p), 2) == 0)
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #define PSEP      '/'
  #define MKDIR(p)  mkdir((p), 0755)
  #define WRITABLE(p) (access((p), W_OK) == 0)
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

static char g_dir[1024];
static char g_path[1200];
static int  g_resolved;      // the directory has been chosen (never means "it exists")
static int  g_make;          // 1 = we picked a fallback that may not exist yet

// Chop the last path component off in place. Returns 0 if there was none.
static int up_one(char *p) {
    char *slash = 0;
    for (char *c = p; *c; c++) if (*c == '/' || *c == '\\') slash = c;
    if (!slash || slash == p) return 0;
    *slash = 0;
    return 1;
}

// The directory the running executable sits in, or 0 if the platform won't say.
static int exe_dir(char *out, size_t n) {
#if defined(__APPLE__)
    char raw[1024]; uint32_t sz = (uint32_t)sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0) return 0;
    // realpath because the path handed back may be relative, may run through symlinks, and —
    // the case that matters — a translocated .app answers with its shadow mount, which is
    // exactly the read-only place we need to detect rather than guess at.
    char real[1024];
    if (!realpath(raw, real)) { size_t l = strlen(raw); if (l >= sizeof real) return 0; memcpy(real, raw, l + 1); }
    if (strlen(real) >= n) return 0;
    memcpy(out, real, strlen(real) + 1);
    if (!up_one(out)) return 0;                      // .../X.app/Contents/MacOS
    // Inside a bundle, "next to the executable" means next to the .app, not buried in it:
    // dropping a save file into Contents/MacOS/ writes INTO the app the user is going to
    // drag to the Trash, and invalidates its signature besides.
    size_t l = strlen(out);
    const char *tail = "/Contents/MacOS";
    size_t t = strlen(tail);
    if (l > t && !strcmp(out + l - t, tail)) {
        out[l - t] = 0;                              // .../X.app
        if (!up_one(out)) return 0;                  // the folder holding X.app
    }
    return 1;
#elif defined(_WIN32)
    DWORD got = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (got == 0 || got >= n) return 0;
    return up_one(out);
#else
    ssize_t got = readlink("/proc/self/exe", out, n - 1);
    if (got <= 0 || (size_t)got >= n - 1) return 0;
    out[got] = 0;
    return up_one(out);
#endif
}

// The per-user data directory this platform keeps for an application, by its own convention.
static int user_dir(char *out, size_t n) {
#if defined(_WIN32)
    const char *base = getenv("APPDATA");
    if (!base || !base[0]) return 0;
    return snprintf(out, n, "%s%c%s", base, PSEP, CVERTEX_NAME) < (int)n;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !home[0]) return 0;
    return snprintf(out, n, "%s/Library/Application Support/%s", home, CVERTEX_NAME) < (int)n;
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') return snprintf(out, n, "%s/%s", xdg, CVERTEX_NAME) < (int)n;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return 0;
    return snprintf(out, n, "%s/.local/share/%s", home, CVERTEX_NAME) < (int)n;
#endif
}

// Pick the directory. Read-only: access() asks the filesystem, it does not touch it.
static void resolve(void) {
    if (g_resolved) return;
    g_resolved = 1;
    char exe[1024];
    exe[0] = 0;
    if (exe_dir(exe, sizeof exe) && WRITABLE(exe)) {          // portable install: files ride along
        snprintf(g_dir, sizeof g_dir, "%s", exe);
        g_make = 0;
        return;
    }
    char usr[1024];
    if (user_dir(usr, sizeof usr)) {
        snprintf(g_dir, sizeof g_dir, "%s", usr);
        g_make = 1;                                            // may not exist yet; created on first write
        return;
    }
    // Neither answered — keep the executable's directory if we have one so the path we hand
    // back is at least meaningful on screen. Writes will fail, honestly and locally.
    snprintf(g_dir, sizeof g_dir, "%s", exe[0] ? exe : ".");
    g_make = 0;
}

// mkdir -p, ignoring "already there". Only ever reached from a for_write path.
static void make_dirs(char *p) {
    for (char *c = p + 1; *c; c++) {
        if (*c != '/' && *c != '\\') continue;
        char save = *c; *c = 0; MKDIR(p); *c = save;
    }
    MKDIR(p);
}

const char *cvx_data_dir(void) { resolve(); return g_dir; }

const char *cvx_data_path(const char *file, int for_write) {
    resolve();
    if (for_write && g_make) { make_dirs(g_dir); g_make = 0; }
    snprintf(g_path, sizeof g_path, "%s%c%s", g_dir, PSEP, file ? file : "");
    return g_path;
}
