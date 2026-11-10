# Super Stardust Ultra VR (CUSA05951) notes

The scene release `Super_Stardust_Ultra_VR_1_03_PS4-LFC.pkg` is a fake PKG of
EP9000-CUSA05951_00-SUPRSTARDULTVREU, category `gd`, APP_VER 01.00, SDK 3.508. It unpacks to
3.5 GB (a 3 GB `stream.dat`), with `libc.prx`, `libSceFios2.prx` and `libSceNpToolkit.prx` in
`sce_module`. The engine is d3t's (not Housemarque's Sasquatch, as RESOGUN): FMOD Ex for audio,
Gnmx compiled into the game.

## What it needed beyond RESOGUN

- **NpToolkit:** `libSceNpToolkit.prx` is linked like libc and Fios2 (its C++ classes are
  imported by the game directly); its own NP imports go to the offline runtime.
- **Import names:** `scripts/gen_import_names.py` with the three modules added 102 names.
- **Stereo buffers:** the game registers its display buffers with
  `sceVideoOutRegisterStereoBuffers` also when no headset is found ("No HMD detected!"). If that
  call fails, the Gnmx context is never set up and the game divides by zero in its submission ring
  (guest 0x499e4f). The video core registers each pair's left-eye buffer.
- **FMOD:** `EventSystem::init` asks for the network profiler, which opens a listening socket.
  Offline sockets used to fail (FMOD_ERR_NET_SOCKET_ERROR, 51), so init failed and the game later
  called through an uninitialized category pointer (guest 0x3418). Offline sockets now open, bind
  and listen, and never get a connection; connect/send/recv still fail.
- **SaveDataMemory2**, `sceRtcParseRFC3339`, HMD (`sceHmdGet2DEyeOffset`: no device), live
  streaming, SharePlay, NP score/TUS/WebApi (offline errors).

## Test (container, Xvfb + lavapipe)

Boots, plays the SIE and d3t intros, the legal screen, title screen "Press ✕", main menu, single
player menu and the arcade difficulty menu in 2D.

## Development helpers added here

- `GOW3_FILE_TRACE=1` logs every file the game opens.
- `GOW3_DEBUG_HOOK=<hex offset>` (Linux) replaces a guest function with one that logs its first
  four arguments, e.g. the game's error reporter at 0x9b40 (FMOD result, expression, file, line).
- `BOOT_PKG_RESUME=1` lets an interrupted unpack go on from where it stopped.
- SIGFPE is reported like the other guest faults.
