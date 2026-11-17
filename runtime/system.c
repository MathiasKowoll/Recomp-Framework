// Small system pieces: winmm timers, the registry (empty), shell folders, user/computer names, and
// leftover kernel32 helpers.
#include "rt.h"
#include "game.h"
#include <sys/stat.h>
#include <time.h>
#include <spawn.h>
#include <strings.h>

// ---------------- winmm ----------------
static uint64_t ms_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
void I_winmm_dll_timeGetTime(CPU *c) { RET((uint32_t)ms_now()); }
void I_winmm_dll_timeBeginPeriod(CPU *c) { RET(0); }   // TIMERR_NOERROR
void I_winmm_dll_timeEndPeriod(CPU *c) { RET(0); }
// timeGetDevCaps(TIMECAPS*: wPeriodMin, wPeriodMax, size)
void I_winmm_dll_timeGetDevCaps(CPU *c) { if (A0 && (uint32_t)A1 >= 8) { wr32(A0, 1); wr32(A0 + 4, 1000000); } RET(0); }

// ---------------- kernel32 leftovers ----------------
// QueryUnbiasedInterruptTime(*ULONGLONG): 100 ns units since boot
void I_kernel32_dll_QueryUnbiasedInterruptTime(CPU *c) { wr64(A0, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 100); RET(1); }
// MulDiv(a, b, c): a*b/c rounded half away from zero, -1 on division by zero
void I_kernel32_dll_MulDiv(CPU *c) {
    int64_t a = (int32_t)A0, b = (int32_t)A1, d = (int32_t)A2;
    if (!d) { RET((uint32_t)-1); return; }
    int64_t p = a * b, r = (p < 0) != (d < 0) ? (p - d / 2) / d : (p + d / 2) / d;
    RET((uint32_t)(int32_t)(r > INT32_MAX || r < INT32_MIN ? -1 : r));
}
void I_kernel32_dll_lstrcpyA(CPU *c) { strcpy(PTR(A0), PTR(A1)); RET(A0); }
void I_kernel32_dll_lstrcpynA(CPU *c) { if ((int32_t)A2 > 0) strlcpy(PTR(A0), PTR(A1), (size_t)(int32_t)A2); RET(A0); }
void I_kernel32_dll_lstrcatA(CPU *c) { strcat(PTR(A0), PTR(A1)); RET(A0); }
void I_kernel32_dll_lstrlenA(CPU *c) { RET(A0 ? strlen(PTR(A0)) : 0); }
void I_kernel32_dll_lstrcmpA(CPU *c) { int r = strcmp(PTR(A0), PTR(A1)); RET((uint32_t)(r < 0 ? -1 : r > 0)); }
void I_kernel32_dll_lstrcmpiA(CPU *c) { int r = strcasecmp(PTR(A0), PTR(A1)); RET((uint32_t)(r < 0 ? -1 : r > 0)); }
static uint32_t error_mode;
void I_kernel32_dll_SetErrorMode(CPU *c) { uint32_t old = error_mode; error_mode = (uint32_t)A0; RET(old); }
void I_kernel32_dll_GetErrorMode(CPU *c) { RET(error_mode); }
// name into a buffer with an in/out size: GetUserName counts the NUL, GetComputerName does not
static int put_name(uint64_t buf, uint64_t psize, const char *s, int count_nul) {
    uint32_t need = (uint32_t)strlen(s) + 1, have = rd32(psize);
    if (have < need) { wr32(psize, need); set_last_error(count_nul ? 122 : 111); return 0; }   // INSUFFICIENT_BUFFER / BUFFER_OVERFLOW
    memcpy(PTR(buf), s, need); wr32(psize, count_nul ? need : need - 1);
    return 1;
}
void I_advapi32_dll_GetUserNameA(CPU *c) { RET(put_name(A0, A1, "Player", 1)); }
void I_kernel32_dll_GetComputerNameA(CPU *c) { RET(put_name(A0, A1, rt_game.computer_name ? rt_game.computer_name : "MAC", 0)); }

// ---------------- advapi32: an empty registry ----------------
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
void I_advapi32_dll_RegOpenKeyExA(CPU *c) { if (AN(4)) wr64(AN(4), 0); RET(ERROR_FILE_NOT_FOUND); }
void I_advapi32_dll_RegOpenKeyExW(CPU *c) { I_advapi32_dll_RegOpenKeyExA(c); }
void I_advapi32_dll_RegOpenKeyA(CPU *c) { if (A2) wr64(A2, 0); RET(ERROR_FILE_NOT_FOUND); }
void I_advapi32_dll_RegCreateKeyExA(CPU *c) { if (AN(7)) wr64(AN(7), 0); RET(ERROR_FILE_NOT_FOUND); }
void I_advapi32_dll_RegQueryValueExA(CPU *c) { RET(ERROR_FILE_NOT_FOUND); }
// RegQueryInfoKeyA(key, class, *classlen, reserved, *subkeys, *maxsubkeylen, *maxclasslen, *values, *maxvaluenamelen,
//                  *maxvaluelen, *secdesclen, *lastwrite): an empty key
void I_advapi32_dll_RegQueryInfoKeyA(CPU *c) {
    if (A2) wr32(A2, 0);
    for (int i = 4; i <= 10; i++) if (AN(i)) wr32(AN(i), 0);
    if (AN(11)) wr64(AN(11), 0);
    RET(0);
}
void I_advapi32_dll_RegEnumValueA(CPU *c) { RET(259); }   // ERROR_NO_MORE_ITEMS

// ---------------- dbghelp.dll: no symbols; the game's crash handler and stack tracer see it unavailable ----------------
void I_dbghelp_dll_SymGetOptions(CPU *c) { RET(0); }
void I_dbghelp_dll_SymSetOptions(CPU *c) { RET((uint32_t)A0); }
void I_dbghelp_dll_SymInitialize(CPU *c) { RET(0); }
void I_dbghelp_dll_SymCleanup(CPU *c) { RET(1); }
void I_dbghelp_dll_StackWalk64(CPU *c) { RET(0); }
void I_dbghelp_dll_SymFromAddr(CPU *c) { RET(0); }
void I_dbghelp_dll_SymGetLineFromAddr64(CPU *c) { RET(0); }
void I_dbghelp_dll_SymGetModuleInfo64(CPU *c) { RET(0); }
void I_dbghelp_dll_SymGetModuleBase64(CPU *c) { RET(0); }
void I_dbghelp_dll_SymFunctionTableAccess64(CPU *c) { RET(0); }
void I_dbghelp_dll_UnDecorateSymbolName(CPU *c) { if (A1 && A2) wr8(A1, 0); RET(0); }
void I_dbghelp_dll_MiniDumpWriteDump(CPU *c) { RET(0); }
void I_advapi32_dll_RegQueryValueExW(CPU *c) { RET(ERROR_FILE_NOT_FOUND); }
void I_advapi32_dll_RegSetValueExA(CPU *c) { RET(ERROR_INVALID_HANDLE); }
void I_advapi32_dll_RegCloseKey(CPU *c) { RET(0); }

// ---------------- shell32: known folders under C:\users\Player ----------------
static void make_dirs(const char *win) {
    char *h = rt_host_path(win);
    if (!h) return;
    for (char *p = h + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(h, 0755); *p = '/'; }
    mkdir(h, 0755);
    free(h);
}
static const char *csidl_path(uint32_t csidl) {
    switch (csidl & 0xFF) {
    case 0x05: return "C:\\users\\Player\\Documents";              // CSIDL_PERSONAL
    case 0x1A: return "C:\\users\\Player\\AppData\\Roaming";       // CSIDL_APPDATA
    case 0x1C: return "C:\\users\\Player\\AppData\\Local";         // CSIDL_LOCAL_APPDATA
    case 0x23: return "C:\\ProgramData";                           // CSIDL_COMMON_APPDATA
    case 0x2E: return "C:\\users\\Public\\Documents";              // CSIDL_COMMON_DOCUMENTS
    case 0x28: return "C:\\users\\Player";                         // CSIDL_PROFILE
    case 0x10: return "C:\\users\\Player\\Desktop";                // CSIDL_DESKTOPDIRECTORY
    case 0x24: return "C:\\Windows";                               // CSIDL_WINDOWS
    case 0x25: return "C:\\Windows\\System32";                     // CSIDL_SYSTEM
    default: return NULL;
    }
}
// SHGetFolderPath(hwnd, csidl, token, flags, path[MAX_PATH])
static void folder_path(CPU *c, int wide) {
    const char *p = csidl_path((uint32_t)A1);
    if (!p) { rt_log("[shell32] SHGetFolderPath(csidl 0x%x) not supported\n", (unsigned)A1); RET(0x80070057u); return; }   // E_INVALIDARG
    make_dirs(p);
    uint64_t out = AN(4);
    if (wide) { uint16_t *w = utf8_to_w16(p); memcpy(PTR(out), w, (w16len(w) + 1) * 2); free(w); }
    else strlcpy(PTR(out), p, 260);
    RET(0);
}
void I_shell32_dll_SHGetFolderPathA(CPU *c) { folder_path(c, 0); }
void I_shell32_dll_SHGetFolderPathW(CPU *c) { folder_path(c, 1); }
// SHGetKnownFolderPath(rfid, flags, token, PWSTR *path): the string is CoTaskMemAlloc'd (malloc here)
void I_shell32_dll_SHGetKnownFolderPath(CPU *c) {
    static const struct { uint32_t d1; const char *path; } known[] = {
        { 0x4C5C32FFu, "C:\\users\\Player\\Saved Games" },           // FOLDERID_SavedGames
        { 0xFDD39AD0u, "C:\\users\\Player\\Documents" },             // FOLDERID_Documents
        { 0xF1B32785u, "C:\\users\\Player\\AppData\\Local" },        // FOLDERID_LocalAppData
        { 0x3EB685DBu, "C:\\users\\Player\\AppData\\Roaming" },      // FOLDERID_RoamingAppData
        { 0x62AB5D82u, "C:\\ProgramData" },                          // FOLDERID_ProgramData
        { 0x5E6C858Fu, "C:\\users\\Player" },                        // FOLDERID_Profile
    };
    uint32_t d1 = A0 ? rd32(A0) : 0;
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].d1 == d1) {
            make_dirs(known[i].path);
            wr64(A3, G(utf8_to_w16(known[i].path)));
            RET(0); return;
        }
    rt_log("[shell32] SHGetKnownFolderPath({%08X-...}) not supported\n", d1);
    wr64(A3, 0);
    RET(0x80004005u);   // E_FAIL
}

// ---------------- starting processes ----------------
// Games start processes mostly to restart themselves (e.g. to switch renderer or game mode: the game's own
// executable with new arguments, then quit) and to open web links. An executable whose name starts with
// rt_game.relaunch_prefix becomes a relaunch of this app with those game arguments; a URL opens in the
// default browser.
int rt_relaunch(int nargs, char **args);
static int split_args(char *s, char **out, int max) {   // Windows command-line rules, simplified: spaces, "quotes"
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char *w = s, *o = s; int q = 0;
        for (; *s && (q || (*s != ' ' && *s != '\t')); s++) { if (*s == '"') q = !q; else *o++ = *s; }
        if (*s) s++;
        *o = 0; out[n++] = w;
    }
    return n;
}
static int is_game_exe(const char *path) {
    const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
    const char *f = strrchr(b, '/'); b = f ? f + 1 : b;
    const char *pre = rt_game.relaunch_prefix ? rt_game.relaunch_prefix : rt_game.exe;
    return !strncasecmp(b, pre, strlen(pre));
}
// relaunch for a command line "exe args...": returns the new pid when it was the game's executable
static int start_game(const char *app, const char *cmdline) {
    char *dup = strdup(cmdline ? cmdline : ""); char *argv[64];
    int n = split_args(dup, argv, 64);
    const char *exe = app && *app ? app : n ? argv[0] : "";
    rt_log("[win32] start process \"%s\" with \"%s\"\n", exe, cmdline ? cmdline : "");
    if (!is_game_exe(exe)) { free(dup); return 0; }
    int skip = (app && *app) ? (n && is_game_exe(argv[0]) ? 1 : 0) : 1;   // the exe name itself is not a game argument
    int pid = rt_relaunch(n - skip, argv + skip);
    free(dup);
    return pid ? pid : -1;
}
static void fill_process_info(uint64_t pi, int pid) {
    if (!pi) return;
    wr64(pi, 0); wr64(pi + 8, 0); wr32(pi + 16, (uint32_t)pid); wr32(pi + 20, 0);
}
// CreateProcess(app, cmdline, procAttr, threadAttr, inherit, flags, env, dir, startupInfo, processInfo)
void I_kernel32_dll_CreateProcessA(CPU *c) {
    int r = start_game(A0 ? (const char *)PTR(A0) : NULL, A1 ? (const char *)PTR(A1) : NULL);
    if (r > 0) { fill_process_info(AN(9), r); RET(1); return; }
    set_last_error(2); RET(0);   // ERROR_FILE_NOT_FOUND
}
void I_kernel32_dll_CreateProcessW(CPU *c) {
    char *app = A0 ? w16_to_utf8(PTR(A0)) : NULL, *cmd = A1 ? w16_to_utf8(PTR(A1)) : NULL;
    int r = start_game(app, cmd);
    free(app); free(cmd);
    if (r > 0) { fill_process_info(AN(9), r); RET(1); return; }
    set_last_error(2); RET(0);
}
// ShellExecute(hwnd, operation, file, parameters, directory, show) -> > 32 on success
static uint64_t shell_execute(const char *file, const char *params) {
    if (!file) return 2;
    if (!strncasecmp(file, "http://", 7) || !strncasecmp(file, "https://", 8)) {
        pid_t pid; char *argv[] = { "/usr/bin/open", (char *)file, NULL };
        extern char **environ;
        rt_log("[win32] open %s\n", file);
        return posix_spawn(&pid, "/usr/bin/open", NULL, NULL, argv, environ) ? 5 : 33;
    }
    char cmd[4096]; snprintf(cmd, sizeof cmd, "\"%s\" %s", file, params ? params : "");
    return start_game(NULL, cmd) > 0 ? 33 : 2;
}
void I_shell32_dll_ShellExecuteA(CPU *c) { RET(shell_execute(A2 ? (const char *)PTR(A2) : NULL, A3 ? (const char *)PTR(A3) : NULL)); }
void I_shell32_dll_ShellExecuteW(CPU *c) {
    char *f = A2 ? w16_to_utf8(PTR(A2)) : NULL, *p = A3 ? w16_to_utf8(PTR(A3)) : NULL;
    uint64_t r = shell_execute(f, p);
    free(f); free(p);
    RET(r);
}

// shlwapi PathFileExists(path): the game checks its own executable before restarting itself
#include <unistd.h>
char *rt_host_path(const char *guest_path);   // files.c: Windows path -> host path (malloc'd)
void I_shlwapi_dll_PathFileExistsA(CPU *c) {
    char *p = A0 ? rt_host_path((const char *)PTR(A0)) : NULL;
    RET(p && access(p, F_OK) == 0);
    free(p);
}
