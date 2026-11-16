#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
// Entry point: load the recompiled game image and run the game's CRT entry point. Game-specific names come from
// rt_game (game.h).
//
//   <App>.app (no arguments): finds the game folder (remembered choice, $<PREFIX>_GAME_DIR, the usual Steam and
//   CrossOver locations, else a folder picker), loads rt_game.exe from it, and logs to
//   ~/Library/Logs/<log_name>.log. Extra arguments are passed to the game.
//
//   Development: boot GAME_EXE GAME_DIR [game args...]
#include "rt.h"
#include "cocoa.h"
#include "game.h"
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

void win32_init(int argc, char **argv);

static const char *home_dir(void) { return getenv("HOME") ? getenv("HOME") : ""; }
static int is_game_dir(const char *d) {
    char p[PATH_MAX];
    if (!d || !*d) return 0;
    snprintf(p, sizeof p, "%s/%s", d, rt_game.exe);
    return access(p, R_OK) == 0;
}
static void config_path(char *out, size_t n) {
    snprintf(out, n, "%s/Library/Application Support/%s/game-dir.txt", home_dir(), rt_game.data_name);
}
static void remember(const char *dir) {
    char p[PATH_MAX]; config_path(p, sizeof p);
    char d[PATH_MAX]; snprintf(d, sizeof d, "%s", p); *strrchr(d, '/') = 0;
    mkdir(d, 0755);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%s\n", dir); fclose(f); }
}
static char *find_game_dir(void) {
    char p[PATH_MAX], line[PATH_MAX];
    config_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        int ok = fgets(line, sizeof line, f) != NULL;
        fclose(f);
        if (ok) { line[strcspn(line, "\n")] = 0; if (is_game_dir(line)) return strdup(line); }
    }
    const char *env = rt_game_env("GAME_DIR");
    if (env && is_game_dir(env)) return strdup(env);
    // Steam (Mac, or the Windows client in a CrossOver bottle), then /Applications/<steam folder>
    const char *patterns[] = {
        "%s/Library/Application Support/Steam/steamapps/common/%s",
        "%s/Library/Application Support/CrossOver/Bottles/*/drive_c/Program Files (x86)/Steam/steamapps/common/%s",
        "%s/Library/Application Support/CrossOver/Bottles/*/drive_c/Program Files/Steam/steamapps/common/%s",
        "%s/Library/Containers/com.isaacmarovitz.Whisky/Bottles/*/drive_c/Program Files (x86)/Steam/steamapps/common/%s",
    };
    for (size_t i = 0; rt_game.steam_folder && i <= sizeof patterns / sizeof *patterns; i++) {
        char pat[PATH_MAX];
        if (i < sizeof patterns / sizeof *patterns) snprintf(pat, sizeof pat, patterns[i], home_dir(), rt_game.steam_folder);
        else snprintf(pat, sizeof pat, "/Applications/%s", rt_game.steam_folder);
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                if (is_game_dir(g.gl_pathv[k])) { char *r = strdup(g.gl_pathv[k]); globfree(&g); remember(r); return r; }
            globfree(&g);
        }
    }
    char msg[512], title[256], text[512];
    if (rt_game.pick_folder_msg) snprintf(msg, sizeof msg, "%s", rt_game.pick_folder_msg);
    else snprintf(msg, sizeof msg, "Choose your %s game folder: the one that contains %s.", rt_game.name, rt_game.exe);
    snprintf(title, sizeof title, "That folder doesn't contain %s", rt_game.name);
    snprintf(text, sizeof text, "Pick the %s folder from your Steam library: it contains %s.", rt_game.name, rt_game.exe);
    for (;;) {
        char *d = cocoa_choose_folder(msg);
        if (!d) return NULL;
        if (is_game_dir(d)) { remember(d); return d; }
        cocoa_alert(title, text);
        free(d);
    }
}

// How this process was started, minus the game arguments: [boot, orig.exe, dir] in a dev build, [app] in the
// app bundle. rt_relaunch() starts a new copy with other game arguments (games restart themselves, e.g. to switch
// renderer or game mode).
static char **boot_prefix; static int boot_nprefix;
int rt_relaunch(int nargs, char **args) {
    char exe[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n)) return 0;
    char **v = calloc((size_t)(boot_nprefix + nargs + 1), sizeof *v);
    int k = 0;
    for (int i = 0; i < boot_nprefix; i++) v[k++] = boot_prefix[i];
    for (int i = 0; i < nargs; i++) v[k++] = args[i];
    v[0] = exe;
    pid_t pid;
    unsetenv("RT_INPUT_SCRIPT");   // a test script drives one session only
    int r = posix_spawn(&pid, exe, NULL, NULL, v, *_NSGetEnviron());
    rt_log("[boot] relaunch: %s (%d game args) -> %s\n", exe, nargs, r ? strerror(r) : "started");
    free(v);
    return r == 0 ? (int)pid : 0;
}

#ifdef __LLVM_INSTR_PROFILE_GENERATE
// PGO builds (recomp.mk PGO=gen): each run writes its profile to ~/Library/Application Support/<data_name>/pgo/
// (RT_PGO_DIR overrides), not into the game folder the game changes to. Merge with `make pgo-merge PGO_DIR=...`.
void __llvm_profile_set_filename(const char *);
static void pgo_output(void) {
    static char p[PATH_MAX];
    char dir[PATH_MAX];
    const char *d = getenv("RT_PGO_DIR");
    if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    else snprintf(dir, sizeof dir, "%s/Library/Application Support/%s/pgo", home_dir(), rt_game.data_name);
    for (char *s = dir + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(dir, 0755); *s = '/'; }
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/run-%%p.profraw", dir);
    __llvm_profile_set_filename(p);
}
#endif

int main(int argc, char **argv) {
#ifdef __LLVM_INSTR_PROFILE_GENERATE
    pgo_output();
#endif
    rt_reexec_fixed_layout(argv);
    int dev = argc >= 3 && argv[1][0] != '-' && argv[1][0] != '+';
    if (!isatty(2)) {   // launched from Finder or by Steam (not a terminal): keep a log file
        char log[PATH_MAX];
        snprintf(log, sizeof log, "%s/Library/Logs/%s.log", getenv("HOME") ? getenv("HOME") : "/tmp", rt_game.log_name);
        // keep the previous sessions' logs: a game that restarts itself spans several processes.
        // <log_name>.prev.log is the last one, .prev2 to .prev4 the ones before.
        int stem = (int)(strlen(log) - 4);
        char a[PATH_MAX], b[PATH_MAX];
        for (int k = 4; k >= 2; k--) {   // prev3 -> prev4, prev2 -> prev3, prev -> prev2
            if (k == 2) snprintf(a, sizeof a, "%.*s.prev.log", stem, log);
            else snprintf(a, sizeof a, "%.*s.prev%d.log", stem, log, k - 1);
            snprintf(b, sizeof b, "%.*s.prev%d.log", stem, log, k);
            rename(a, b);
        }
        char prev[PATH_MAX]; snprintf(prev, sizeof prev, "%.*s.prev.log", stem, log);
        rename(log, prev);
        freopen(log, "w", stderr);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    rt_init();

    char exe[PATH_MAX]; const char *dir;
    int gargc; char **gargv;
    // absolute paths: the game chdirs into its folder before it ever relaunches
    boot_nprefix = dev ? 3 : 1;
    boot_prefix = calloc(4, sizeof *boot_prefix);
    for (int i = 0; i < boot_nprefix; i++) { char rp[PATH_MAX]; boot_prefix[i] = strdup(realpath(argv[i], rp) ? rp : argv[i]); }
    if (dev) {
        snprintf(exe, sizeof exe, "%s", argv[1]);
        dir = argv[2];
        gargc = argc - 2; gargv = argv + 2;
    } else {
        char *d = find_game_dir();
        if (!d) return 0;   // picker cancelled
        dir = d;
        snprintf(exe, sizeof exe, "%s/%s", d, rt_game.exe);
        // game arguments: everything except macOS launch noise (-psn_..., -NSDocumentRevisionsDebugMode ...)
        gargv = calloc((size_t)argc + 1, sizeof *gargv); gargc = 0;
        gargv[gargc++] = argv[0];
        for (int i = 1; i < argc; i++) {
            if (!strncmp(argv[i], "-psn_", 5)) continue;
            if (!strncmp(argv[i], "-NS", 3) && i + 1 < argc) { i++; continue; }
            gargv[gargc++] = argv[i];
        }
    }
    if (rt_game.extra_args) {   // arguments the game always gets (game.c), after the user's
        int ne = 0; while (rt_game.extra_args[ne]) ne++;
        char **v = calloc((size_t)(gargc + ne + 1), sizeof *v);
        memcpy(v, gargv, (size_t)gargc * sizeof *v);
        for (int i = 0; i < ne; i++) v[gargc++] = (char *)rt_game.extra_args[i];
        gargv = v;
    }
    rt_log("[boot] game folder %s\n", dir);
    rt_load_image(exe);
    if (chdir(dir)) { perror(dir); return 1; }
    win32_init(gargc, gargv);
    CPU *c = cpu_new(8u << 20);
    rt_set_cur(c);
    rt_log("[boot] image at 0x%llx, entry 0x%llx, %d functions\n", (unsigned long long)gimage_base, (unsigned long long)gentry, ngfuncs);
    uint64_t r = guest_call(c, gentry, 0, NULL);
    rt_log("[boot] entry returned %llu\n", (unsigned long long)r);
    return 0;
}
