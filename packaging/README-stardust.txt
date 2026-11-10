Super Stardust Ultra for PC - native port of the PS4 game (Windows and Linux / SteamOS)

No game files are included. You need your own Super Stardust Ultra VR for PS4 (CUSA05951) as a
fake PKG. It plays as the regular 2D game: no PS VR headset is used.

FIRST START
  1. Put your PKG file in this folder, next to stardust.exe (Linux: stardust).
  2. Run stardust.exe (Linux: ./stardust).
  3. The first start unpacks the PKG into the "game" folder here (about 3.5 GB, a minute or two)
     and prepares the game once. After that it starts straight away, and the PKG can be
     deleted or moved.
  Already have the game unpacked? A folder named "game" here (with eboot.bin in it) works too.

SETTINGS
  In the game: Insert on the keyboard, or R3 + L2 on a controller. Saved in stardust.ini.

CONTROLS
  Any controller works. Keyboard: Space = Cross, Left Shift = Circle, E = Square,
  Enter = Options, I / J / K / L = d-pad.

FILES MADE HERE
  game/          the unpacked game
  out/           the prepared program (made again when the game files change)
  user/          saves, shader caches and last_run.log
  stardust.ini   settings

STEAM DECK / LINUX
  Unpack the folder anywhere, put the PKG in it, run ./stardust once from the desktop.
  For Gaming Mode: Steam > Add a Non-Steam Game > browse to stardust, then in the shortcut's
  Properties > Compatibility choose "Steam Linux Runtime" (not Proton).

PROBLEMS
  Every start writes user/last_run.log. Please attach it to a bug report:
  https://github.com/darkuni/super-stardust-ultra-recomp/issues
