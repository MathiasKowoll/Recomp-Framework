#!/usr/bin/env python3
"""Register a recompiled game as a Steam Play compatibility tool in Steam for Mac.

Steam for Mac ships the same compatibility-tool support as Steam on Linux, but leaves it switched off.
NotProton (github.com/NotProtonNot/NotProton) switches it on. Steam then scans
~/Library/Application Support/Steam/compatibilitytools.d, and a tool there that runs Windows apps on macOS can
be chosen for a game (library: the game > Properties > Compatibility > Force the use of a specific Steam Play
compatibility tool). Steam then downloads the game's Windows release into the Mac library, and Play on the
game's own library entry starts the native app on those files. Steam treats it as the real game: playtime,
friends presence, invites, cloud saves and the Steam overlay. Steam passes its overlay libraries in
STEAM_DYLD_INSERT_LIBRARIES; the launcher moves them into DYLD_INSERT_LIBRARIES for the game.

Without NotProton the folder is inert, and the game works as before.

The overlay libraries have to be inserted with DYLD_INSERT_LIBRARIES, and macOS strips DYLD_* variables when it
starts a system binary such as /bin/sh. So the tool's `run` entry point is a small compiled launcher that sets
the variable and execs the app directly, not a shell script. It needs clang (Apple's Command Line Tools). Only Python's standard library is
used, so the system Python can run this script.

usage: steam_play_tool.py --app /Applications/Game.app --id game_recomp --display "Game Recomp"
                          --exe 'game*.exe' [--force-exe Game.exe] [--remove]
  --app        the recompiled game's .app (its Contents/MacOS executable is started)
  --id         the tool's internal name (letters, digits and _), also its folder name
  --display    the name shown in Steam's compatibility tool list
  --exe        which executables Steam starts are the game (a glob, case-insensitive). Anything else Steam runs
               first (redistributable installers and other helpers) finishes at once: there is nothing to
               install on macOS.
  --force-exe  always start this executable from the game folder, whichever launch option Steam picked
  --remove     unregister the tool
"""
import os, shutil, subprocess, sys, tempfile

TOOLS = os.path.expanduser("~/Library/Application Support/Steam/compatibilitytools.d")

DECLARATION = '''"compatibilitytools"
{
  "compat_tools"
  {
    "%(id)s"
    {
      "install_path" "."
      "display_name" "%(display)s"
      "from_oslist" "windows"
      "to_oslist" "macos"
    }
  }
}
'''

MANIFEST = '''"manifest"
{
  "version" "2"
  "commandline" "/run %verb%"
}
'''

# Steam runs:  run <verb> <Windows executable> [arguments]
LAUNCHER = r'''#include <fnmatch.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

extern char **environ;

static const char *APP_BIN = "@BIN@", *EXE_GLOB = "@GLOB@", *FORCE_EXE = "@FORCE@";

int main(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *verb = argv[1];
    {   /* what Steam asked for, for troubleshooting: ~/Library/Logs/steam-play-<id>.log (latest launch) */
        char lp[PATH_MAX];
        const char *home = getenv("HOME");
        snprintf(lp, sizeof lp, "%s/Library/Logs/steam-play-@ID@.log", home ? home : "/tmp");
        FILE *f = fopen(lp, strcmp(verb, "run") && strcmp(verb, "waitforexitandrun") ? "a" : "w");
        if (f) {
            for (int i = 0; i < argc; i++) fprintf(f, "argv[%d] = %s\n", i, argv[i]);
            for (char **e = environ; *e; e++)
                if (!strncmp(*e, "DYLD_", 5) || !strncasecmp(*e, "steam", 5) || strstr(*e, "OVERLAY")) fprintf(f, "%s\n", *e);
            fprintf(f, "\n");
            fclose(f);
        }
    }
    if (!strcmp(verb, "getcompatpath") || !strcmp(verb, "getnativepath")) {
        if (argc > 2) printf("%s\n", argv[2]);
        return 0;
    }
    if (strcmp(verb, "run") && strcmp(verb, "waitforexitandrun") && strcmp(verb, "waitforexit")) return 0;
    if (argc < 3) return 0;
    char path[PATH_MAX], dir[PATH_MAX], base[PATH_MAX], exe[PATH_MAX];
    snprintf(path, sizeof path, "%s", argv[2]);
    snprintf(dir, sizeof dir, "%s", dirname(path));
    snprintf(path, sizeof path, "%s", argv[2]);
    snprintf(base, sizeof base, "%s", basename(path));
    if (fnmatch(EXE_GLOB, base, FNM_CASEFOLD) != 0) return 0;   /* a helper Steam runs first */
    if (*FORCE_EXE) snprintf(exe, sizeof exe, "%s/%s", dir, FORCE_EXE);
    else snprintf(exe, sizeof exe, "%s", argv[2]);
    if (access(APP_BIN, X_OK) != 0) {
        execl("/usr/bin/osascript", "osascript", "-e",
              "display alert \"@DISPLAY@ is not installed\" message \"Install it again, or choose another "
              "compatibility tool for this game in Steam.\"", (char *)NULL);
        return 1;
    }
    /* the app's command line: <Windows executable> <game folder> [game arguments] */
    char *args[argc + 2];
    int n = 0;
    args[n++] = (char *)APP_BIN;
    args[n++] = exe;
    args[n++] = dir;
    for (int i = 3; i < argc; i++) args[n++] = argv[i];
    args[n] = NULL;
    /* Steam hands its overlay libraries (steamloader.dylib, gameoverlayrenderer.dylib) to the launcher as
     * STEAM_DYLD_INSERT_LIBRARIES; the launcher is expected to insert them into the game. */
    const char *ov = getenv("STEAM_DYLD_INSERT_LIBRARIES");
    if (ov && *ov) {
        const char *cur = getenv("DYLD_INSERT_LIBRARIES");
        if (cur && *cur && !strstr(cur, ov)) {
            char both[8192];
            snprintf(both, sizeof both, "%s:%s", cur, ov);
            setenv("DYLD_INSERT_LIBRARIES", both, 1);
        } else if (!cur || !*cur) setenv("DYLD_INSERT_LIBRARIES", ov, 1);
    }
    execv(APP_BIN, args);   /* keeps the environment, so the overlay libraries reach the game */
    perror("execv");
    return 1;
}
'''


def c_str(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    a = sys.argv[1:]
    def opt(name, default=None):
        return a[a.index(name) + 1] if name in a else default
    tool_id = opt("--id")
    if not tool_id or not tool_id.replace("_", "").isalnum():
        sys.exit("--id is required (letters, digits and _)")
    tool = os.path.join(TOOLS, tool_id)
    if "--remove" in a:
        shutil.rmtree(tool, ignore_errors=True)
        print("removed", tool)
        return 0
    app = os.path.abspath(opt("--app", ""))
    display, glob, force = opt("--display", tool_id), opt("--exe"), opt("--force-exe", "")
    if not glob:
        sys.exit("--exe is required")
    macos = os.path.join(app, "Contents", "MacOS")
    names = sorted(os.listdir(macos)) if os.path.isdir(macos) else []
    if not names:
        sys.exit("no executable in %s" % macos)
    binary = os.path.join(macos, names[0])
    src = (LAUNCHER.replace("@BIN@", c_str(binary)).replace("@GLOB@", c_str(glob))
           .replace("@FORCE@", c_str(force)).replace("@ID@", tool_id).replace("@DISPLAY@", c_str(display).replace('\\"', "")))
    os.makedirs(tool, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "run.c")
        open(c, "w").write(src)
        out = os.path.join(tmp, "run")
        r = subprocess.run(["clang", "-O2", "-o", out, c], capture_output=True, text=True)
        if r.returncode:
            shutil.rmtree(tool, ignore_errors=True)
            sys.exit("clang failed:\n" + r.stderr)
        shutil.copy2(out, os.path.join(tool, "run"))
    open(os.path.join(tool, "compatibilitytool.vdf"), "w").write(DECLARATION % {"id": tool_id, "display": display})
    open(os.path.join(tool, "toolmanifest.vdf"), "w").write(MANIFEST)
    print("registered %s (runs %s)" % (tool, binary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
