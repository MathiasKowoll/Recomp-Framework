// Game settings for Recomp-Framework (see runtime/game.h). Every name the runtime needs for a particular game
// comes from here.
#include "game.h"

const RtGame rt_game = {
    .name = "My Game",
    .exe = "MyGame.exe",                // the Windows executable in the game folder
    .win_dir = "C:\\Games\\MyGame",     // where the game believes it is installed (no trailing backslash)
    .data_name = "mygame-recomp",       // ~/Library/Application Support/mygame-recomp: the other drives, saves, settings
    .log_name = "MyGame-recomp",        // ~/Library/Logs/MyGame-recomp.log
    .env_prefix = "MYGAME",             // MYGAME_GAME_DIR, MYGAME_DATA_DIR, MYGAME_STEAM_OFFLINE, MYGAME_CLOUD ...
    .steam_folder = "MyGame",           // steamapps/common/MyGame, for finding the game
    .steam_app_id = 0,                  // the game's Steam app ID (0: no Steam)
    .relaunch_prefix = "MyGame",        // the game starting an exe named MyGame* relaunches the app
    .extra_args = NULL,                 // arguments always added to the command line, e.g. (const char *[]){ "+x", "1", NULL }
    .cloud_save_dir = NULL,             // save folder synced to Steam Cloud, e.g. "c/users/Player/Saved Games/MyGame/%llu"
    .cloud_prefix = NULL,               // e.g. "mygame-recomp/"
    .computer_name = "MAC",
    .pick_folder_msg = NULL,            // default: "Choose your My Game game folder: the one that contains MyGame.exe."
};
