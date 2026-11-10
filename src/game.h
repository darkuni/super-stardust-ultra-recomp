/* The game this program runs: the only file that differs between the recomps built from this
 * runtime (RESOGUN, Super Stardust Ultra). */
#pragma once

#define GAME_TITLE "Super Stardust Ultra"
/* Program, settings and Linux application id. */
#define GAME_PROGRAM "stardust"
/* PKGs of other games next to the program are skipped: a PKG is this game's when its title id
 * is one of these, or its title contains GAME_TITLE_MATCH. */
#define GAME_TITLE_IDS {"CUSA05951", "CUSA06024", "CUSA06006", "CUSA06009", "CUSA00952", "CUSA01560", NULL}
#define GAME_TITLE_MATCH "Stardust"
#define GAME_PKG_HINT "the game PKG (Super Stardust Ultra or Super Stardust Ultra VR)"
/* Modules from sce_module linked into the executable (the rest are the host runtime's). */
#define GAME_MODULES {"libc.prx", "libSceFios2.prx", "libSceNpToolkit.prx", NULL}
