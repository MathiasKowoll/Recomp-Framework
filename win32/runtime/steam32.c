// steam_api.dll for 32-bit guests. Each interface the game asks for (SteamInternal_FindOrCreateUserInterface /
// SteamInternal_CreateInterface by version string) is a guest object whose vtable slots are native functions; the
// slot table (steam32_slots.h, generated from the SDK's steam_api.json) says how many stack bytes each 32-bit
// thiscall method pops and how it returns. Methods are implemented by name, so one implementation serves every
// interface version.
//
// Online (the Steam for Mac client is running and logged in): identity calls go to Valve's libsteam_api.dylib
// through its flat API -- the Steam ID, logged-on state, persona name, language, DLC and subscription checks.
// Offline (no client, or <RT_STEAM_OFFLINE>): as the x86-64 framework does, SteamAPI_Init succeeds, the user is
// "Player" and logged on, and everything else answers 0.
#include "rt32.h"
#include "steam32_slots.h"
#include <dlfcn.h>
#include <strings.h>

typedef struct { const SteamIface *ifc; int slot; } SlotCtx;
typedef struct { const SteamIface *ifc; uint32_t guest; } Obj;
static Obj objs[64]; static int nobjs;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

// ---------------- the real client ----------------
static struct {
    int online, tried;
    void *user, *friends, *utils, *apps, *stats;
    int (*InitSafe)(void);
    void (*RunCallbacks)(void);
    int (*BLoggedOn)(void *);
    uint64_t (*GetSteamID)(void *);
    const char *(*GetPersonaName)(void *);
    uint32_t (*GetAppID)(void *);
    const char *(*GetCurrentGameLanguage)(void *);
    int (*BIsDlcInstalled)(void *, uint32_t);
    int (*BIsSubscribedApp)(void *, uint32_t);
    int (*BIsSubscribed)(void *);
    const char *(*GetIPCountry)(void *);
} S;

static void *steam_lib(void) {
    const char *env = getenv("RT_STEAM_API");
    if (env) { void *h = dlopen(env, RTLD_NOW | RTLD_LOCAL); if (h) return h; }
    char p[2048];
    snprintf(p, sizeof p, "%s/../Frameworks/libsteam_api.dylib", rt_exe_dir);
    void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    const char *home = getenv("HOME") ? getenv("HOME") : "";
    const char *known[] = {
        "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib",
        "%s/Library/Application Support/Steam/steamapps/common/Half-Life/libsteam_api.dylib",
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
        snprintf(p, sizeof p, known[i], home);
        if ((h = dlopen(p, RTLD_NOW | RTLD_LOCAL))) { rt_log("[steam] using %s", p); return h; }
    }
    return NULL;
}
static void *accessor(void *h, const char *iface, int newest, int oldest) {
    char n[96];
    for (int v = newest; v >= oldest; v--) {
        snprintf(n, sizeof n, "SteamAPI_%s_v%03d", iface, v);
        void *f = dlsym(h, n);
        if (f) return f;
    }
    return NULL;
}
static void connect_client(void) {
    if (S.tried) return;
    S.tried = 1;
    if (getenv("RT_STEAM_OFFLINE")) { rt_log("[steam] offline (RT_STEAM_OFFLINE)"); return; }
    void *h = steam_lib();
    if (!h) { rt_log("[steam] libsteam_api.dylib not found: offline"); return; }
#define SYM(field, name) *(void **)&S.field = dlsym(h, name); if (!S.field) { rt_log("[steam] %s missing: offline", name); return; }
    SYM(InitSafe, "SteamAPI_InitSafe") SYM(RunCallbacks, "SteamAPI_RunCallbacks")
    SYM(BLoggedOn, "SteamAPI_ISteamUser_BLoggedOn") SYM(GetSteamID, "SteamAPI_ISteamUser_GetSteamID")
    SYM(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName") SYM(GetAppID, "SteamAPI_ISteamUtils_GetAppID")
    SYM(GetCurrentGameLanguage, "SteamAPI_ISteamApps_GetCurrentGameLanguage")
    SYM(BIsDlcInstalled, "SteamAPI_ISteamApps_BIsDlcInstalled") SYM(BIsSubscribedApp, "SteamAPI_ISteamApps_BIsSubscribedApp")
    SYM(BIsSubscribed, "SteamAPI_ISteamApps_BIsSubscribed") SYM(GetIPCountry, "SteamAPI_ISteamUtils_GetIPCountry")
#undef SYM
    void *(*user_v)(void) = accessor(h, "SteamUser", 24, 21), *(*friends_v)(void) = accessor(h, "SteamFriends", 18, 15);
    void *(*utils_v)(void) = accessor(h, "SteamUtils", 11, 9), *(*apps_v)(void) = accessor(h, "SteamApps", 9, 8);
    if (!user_v || !friends_v || !utils_v || !apps_v) { rt_log("[steam] libsteam_api.dylib is from an unsupported SDK: offline"); return; }
    char id[16]; snprintf(id, sizeof id, "%u", rt_game.steam_app_id);
    setenv("SteamAppId", id, 1); setenv("SteamGameId", id, 1);
    if (!rt_game.steam_app_id || !S.InitSafe()) { rt_log("[steam] Steam client not running (or not logged in): offline"); return; }
    S.user = user_v(); S.friends = friends_v(); S.utils = utils_v(); S.apps = apps_v();
    S.online = S.user && S.friends && S.utils && S.apps;
    rt_log("[steam] online through the Steam client (app %u)", rt_game.steam_app_id);
}

// ---------------- method implementations (by interface and method name) ----------------
typedef struct { CPU *c; uint32_t self; uint32_t sret; } Call;
#define A(i) rds32(c->esp + 4 + 4 * (i) + (call->sret ? 4 : 0))   // i-th declared argument (after a hidden return pointer)
typedef uint64_t (*Impl)(CPU *c, Call *call);

static uint64_t ret1(CPU *c, Call *call) { return 1; }
static uint64_t u_BLoggedOn(CPU *c, Call *call) { return S.online ? (uint64_t)(S.BLoggedOn(S.user) != 0) : 1; }
static uint64_t u_GetSteamID(CPU *c, Call *call) {
    uint64_t id = S.online ? S.GetSteamID(S.user) : 0x0110000100000001ull;   // offline: individual account 1
    wr64(call->sret, id);
    return call->sret;
}
static uint64_t u_GetHSteamUser(CPU *c, Call *call) { return 1; }
static uint32_t persona;
static uint64_t f_GetPersonaName(CPU *c, Call *call) {
    if (!persona) persona = gstrdup(S.online ? S.GetPersonaName(S.friends) : "Player");
    return persona;
}
static uint64_t ut_GetAppID(CPU *c, Call *call) { return S.online ? S.GetAppID(S.utils) : rt_game.steam_app_id; }
static uint32_t country;
static uint64_t ut_GetIPCountry(CPU *c, Call *call) { if (!country) country = gstrdup(S.online ? S.GetIPCountry(S.utils) : "US"); return country; }
static uint32_t language;
static uint64_t a_GetCurrentGameLanguage(CPU *c, Call *call) {
    if (!language) language = gstrdup(S.online ? S.GetCurrentGameLanguage(S.apps) : "english");
    return language;
}
static uint64_t a_BIsDlcInstalled(CPU *c, Call *call) { return S.online ? (uint64_t)(S.BIsDlcInstalled(S.apps, A(0)) != 0) : 0; }
static uint64_t a_BIsSubscribedApp(CPU *c, Call *call) {
    if (S.online) return (uint64_t)(S.BIsSubscribedApp(S.apps, A(0)) != 0);
    return A(0) == rt_game.steam_app_id;
}
static uint64_t a_BIsSubscribed(CPU *c, Call *call) { return S.online ? (uint64_t)(S.BIsSubscribed(S.apps) != 0) : 1; }

typedef struct { const char *iface, *method; Impl fn; } ImplRow;
static const ImplRow impls[] = {
    {"ISteamUser", "GetHSteamUser", u_GetHSteamUser}, {"ISteamUser", "BLoggedOn", u_BLoggedOn},
    {"ISteamUser", "GetSteamID", u_GetSteamID},
    {"ISteamFriends", "GetPersonaName", f_GetPersonaName},
    {"ISteamUtils", "GetAppID", ut_GetAppID}, {"ISteamUtils", "GetIPCountry", ut_GetIPCountry},
    {"ISteamUtils", "IsOverlayEnabled", NULL}, {"ISteamUtils", "BOverlayNeedsPresent", NULL},
    {"ISteamApps", "GetCurrentGameLanguage", a_GetCurrentGameLanguage}, {"ISteamApps", "GetAvailableGameLanguages", a_GetCurrentGameLanguage},
    {"ISteamApps", "BIsDlcInstalled", a_BIsDlcInstalled}, {"ISteamApps", "BIsSubscribedApp", a_BIsSubscribedApp},
    {"ISteamApps", "BIsSubscribed", a_BIsSubscribed}, {"ISteamApps", "BIsAppInstalled", a_BIsSubscribedApp},
    // stats: accept, report success (no achievements offline yet; online bridging comes with callbacks)
    {"ISteamUserStats", "RequestCurrentStats", ret1}, {"ISteamUserStats", "StoreStats", ret1},
    {"ISteamUserStats", "SetAchievement", ret1}, {"ISteamUserStats", "ClearAchievement", ret1},
    {"ISteamUserStats", "SetStat", ret1}, {"ISteamUserStats", "UpdateAvgRateStat", ret1},
};

static void slot_call(CPU *c, uintptr_t ctx) {
    const SlotCtx *sc = (const SlotCtx *)ctx;
    const SteamSlot *s = &sc->ifc->slots[sc->slot];
    Call call = {c, c->ecx, s->ret == 'S' ? ARG(0) : 0};
    Impl fn = NULL; int found = 0;
    for (size_t i = 0; i < sizeof impls / sizeof *impls; i++)
        if (!strcmp(impls[i].iface, sc->ifc->iface) && !strcmp(impls[i].method, s->name)) { fn = impls[i].fn; found = 1; break; }
    uint64_t r = 0;
    if (fn) r = fn(c, &call);
    else if (!found) {
        static const void *seen[512]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == s) known = 1;
        if (!known && nseen < 512) { seen[nseen++] = s; rt_log("[steam] %s::%s (%s slot %d) -> 0", sc->ifc->iface, s->name, sc->ifc->version, sc->slot); }
        if (s->ret == 'S') { memset(G2H(call.sret), 0, 8); r = call.sret; }
    }
    switch (s->ret) {
    case 'q': c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32); break;
    case 'f': FPUSH(c, 0.0); break;
    case 'v': break;
    default: c->eax = (uint32_t)r; break;
    }
    c->esp += s->argbytes;   // thiscall: the callee pops its arguments (call_ind_slow pops the return address)
}

// find the table for a version string; a newer version of a known interface falls back to the newest we have
static const SteamIface *find_iface(const char *ver) {
    for (int i = 0; i < NSTEAM_IFACES; i++) if (!strcasecmp(steam_ifaces[i].version, ver)) return &steam_ifaces[i];
    // family match: same name without the trailing number ("SteamFriends018" -> "SteamFriends017")
    size_t n = strlen(ver); while (n && ver[n - 1] >= '0' && ver[n - 1] <= '9') n--;
    const SteamIface *best = NULL;
    for (int i = 0; i < NSTEAM_IFACES; i++)
        if (!strncasecmp(steam_ifaces[i].version, ver, n) && strlen(steam_ifaces[i].version) > n) best = &steam_ifaces[i];
    if (best) rt_log("[steam] no table for %s: using %s's (check the slots the game calls)", ver, best->version);
    // ISteamClient's json entry has no version string
    if (!best && !strncasecmp(ver, "SteamClient", 11))
        for (int i = 0; i < NSTEAM_IFACES; i++) if (!strcmp(steam_ifaces[i].iface, "ISteamClient")) best = &steam_ifaces[i];
    return best;
}

static uint32_t make_iface(const char *ver) {
    pthread_mutex_lock(&mu);
    const SteamIface *ifc = find_iface(ver);
    if (!ifc) { pthread_mutex_unlock(&mu); rt_log("[steam] unknown interface %s -> NULL", ver); return 0; }
    for (int i = 0; i < nobjs; i++) if (objs[i].ifc == ifc) { pthread_mutex_unlock(&mu); return objs[i].guest; }
    uint32_t vt = gcalloc((uint32_t)ifc->n, 4);
    for (int s = 0; s < ifc->n; s++) {
        SlotCtx *sc = malloc(sizeof *sc); sc->ifc = ifc; sc->slot = s;
        char *nm = malloc(96); snprintf(nm, 96, "%s::%s", ifc->iface, ifc->slots[s].name);
        wr32(vt + 4u * (uint32_t)s, rt_host_fn_ctx(slot_call, (uintptr_t)sc, nm));
    }
    uint32_t obj = gcalloc(1, 16);
    wr32(obj, vt);
    if (nobjs < 64) objs[nobjs++] = (Obj){ifc, obj};
    pthread_mutex_unlock(&mu);
    if (rt_verbose) rt_log("[steam] interface %s (%s, %d slots) -> 0x%x", ver, ifc->iface, ifc->n, obj);
    return obj;
}

// ---------------- flat exports (cdecl) ----------------
// SteamInternal_SteamAPI_Init(const char *versions, SteamErrMsg *err) -> ESteamAPIInitResult (0 = OK)
CSHIM(steam_api_SteamInternal_SteamAPI_Init) { connect_client(); if (ARG(1)) wr8(ARG(1), 0); return 0; }
CSHIM(steam_api_SteamAPI_IsSteamRunning) { connect_client(); return 1; }
CSHIM(steam_api_SteamAPI_Shutdown) { return 0; }
CSHIM(steam_api_SteamAPI_RunCallbacks) { if (S.online) S.RunCallbacks(); return 0; }
CSHIM(steam_api_SteamAPI_GetHSteamUser) { return 1; }
// ContextInit(ContextInitData *d): d = {void (*init)(void *ctx), counter, CSteamAPIContext ctx}; the SDK's inline
// accessors call it on every use: the first time (counter differs) it runs `init`, which fills ctx through
// SteamInternal_FindOrCreateUserInterface. Returns &d->ctx.
static uint32_t ctx_counter = 1;
CSHIM(steam_api_SteamInternal_ContextInit) {
    uint32_t d = ARG(0);
    if (!d) return 0;
    if (rd32(d + 4) != ctx_counter) {
        wr32(d + 4, ctx_counter);
        uint32_t arg = d + 8;
        guest_call(c, rd32(d), 1, &arg);
    }
    return d + 8;
}
CSHIM(steam_api_SteamInternal_FindOrCreateUserInterface) { return STR(1) ? make_iface(STR(1)) : 0; }
CSHIM(steam_api_SteamInternal_CreateInterface) { return STR(0) ? make_iface(STR(0)) : 0; }
// callbacks the game registers (CCallbackBase objects): kept for when the client posts them; offline none fire
CSHIM(steam_api_SteamAPI_RegisterCallback) { if (rt_verbose) rt_log("[steam] RegisterCallback(0x%x, %u)", ARG(0), ARG(1)); return 0; }
CSHIM(steam_api_SteamAPI_UnregisterCallback) { return 0; }
CSHIM(steam_api_SteamAPI_RegisterCallResult) { return 0; }
CSHIM(steam_api_SteamAPI_UnregisterCallResult) { return 0; }
