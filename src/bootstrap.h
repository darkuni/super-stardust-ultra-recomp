/* The program (stardust / stardust.exe) started by itself (double-click, Steam): find the game next to the
 * program, unpack its PKGs and prepare it on the first start, then hand the loader the arguments
 * run.py used to build. Replaces the Python launcher of the ports this project comes from. */
#pragma once

/* 1 when argv asks for the self-contained start: no arguments, or only bootstrap options
 * (--after PID, --timeout N, --check-imports). */
int bootstrap_wanted(int argc, char **argv);
/* Prepares everything and replaces argc and argv with the loader's arguments. Returns 0 to go on,
 * or an exit code (an error was shown to the player). */
int bootstrap(int *argc, char ***argv);
/* This CPU cannot run the game: a message box naming the missing instruction sets. */
void bootstrap_cpu_message(const char *missing, int fatal);
/* The game process started by bootstrap() (Windows) writes to the same last_run.log. */
void bootstrap_game_log(void);
