Twisted Metal: Head-On - Extra Twisted Edition - native PC port
=================================================================

Getting started
  1. Double-click TMHO.exe. The game setup screen opens.
  2. Choose your own disc image of the game (SCUS-97621, a .iso file) with
     "Browse...", or drag the .iso onto the window. A disc image next to
     TMHO.exe, or in a "Game Rom" folder one or two levels up, is found by
     itself - and so is the disc another copy of the port on this PC used.
  3. Click "Build game". It copies the game's files from your disc into a
     "game" folder next to TMHO.exe (about 4.1 GB - keep that much space
     free) and checks that the game program is the version this port was
     made from. If another copy of the port on this PC was built before
     (an older version folder, say), Build reuses its files instead: that
     takes no extra disk space.
  4. Optional: click "Build shaders". It prepares all 10,904 of the game's
     GPU shader variants now and saves them as shadercache.bin next to
     TMHO.exe (a few seconds; longer right after a graphics driver update),
     so starting the game doesn't have to compile them and no effect
     stutters the first time it appears. Skipped, the game builds them in
     the background while you play and (V9) saves the file as soon as they
     are all built; any new ones are added when you quit. Build them again
     after a graphics driver update.
  5. Click PLAY.

From then on TMHO.exe shows the setup screen with PLAY ready; turn off
"Show this screen at start" to go straight into the game. To pick another
disc later: F2 > Game > Game disc > Setup.

Keep the .iso where it is - the game still streams its music and movies
from it. Saves go in the "saves" folder next to TMHO.exe.

Keys (or plug in a controller)
  Arrows / WASD     D-pad          Enter        Start
  X or Space        Cross          Tab          Select
  C or Esc          Circle         Q / E        L1 / R1
  Z                 Square         Left/Right Shift   L2 / R2
  V                 Triangle
  F2                settings (video, audio, game, HUD, UI colour); with a
                    controller: the Guide / PS button, or press both sticks
                    in together, then the D-pad, A / Cross and B / Circle
  F11 or Alt+Enter  windowed <-> full screen
  F12               screenshot (Screenshots folder next to TMHO.exe)
  F1                debugger

To quit, close the window, or use F2 > Game > Quit game (press Quit twice).

Video (F2 > Video) - all of it changes straight away, no restart needed
  Display mode         Windowed, Borderless or Fullscreen. F11 or Alt+Enter
                       switches between windowed and full screen at any time.
  Resolution           The game picture's resolution, whatever your screen is:
                       480p (1x, the PS2's own), 720p, 1080p, 1440p or 2160p (4K).
                       Higher is sharper; lower is lighter on the graphics card.
                       One above your screen's is drawn big and shrunk to fit,
                       for smoother edges.
  Aspect ratio         4:3 is the original picture. 16:9 is true widescreen: the
                       game's 3D cameras see more to each side (nothing is
                       stretched), and the HUD, radar and text keep their shape.
                       Pre-rendered movies stay 4:3. Stretch fills the screen.
  Scaling              Sharp or Smooth, when the picture is smaller than the
                       screen.
  V-Sync               Waits for the monitor's refresh to avoid tearing.
  Frame rate           60 fps shows the game's own frames. 120 fps and Monitor
                       (your screen's refresh rate, e.g. 144 or 170) add
                       in-between frames made from the two newest game frames:
                       motion is smoother and the game keeps its normal speed.
                       The picture is then about one frame later.
  Brightness           Lightens (or darkens) the picture, 70-150 %. Dark parts
                       change most, so night arenas get easier to see without
                       washing out explosions. 100 % is the game's own picture.
  Sharpening           (V9) Makes fine detail and text crisper after the
                       picture is scaled, 0-100 %. Strongest on soft detail,
                       gentle on edges that are already sharp. 0 % is off.

In windowed mode the window opens where you last left it, at the size you
left it.

On a slower PC: Resolution 480p or 720p, Frame rate 60 fps, and keep
F2 > Game > VU1 thread and IOP thread on (the defaults).

Sound (F2 > Audio)
  Master volume        Everything the game plays.
  Music volume         The music (and other sound the game streams from the
                       disc, such as story voice-overs).
  Effects volume       Engines, weapons, explosions, pickups, menu sounds and
                       the announcer.
  Movie volume         The intro and ending movies.

Controls (F2 > Controls) - V9
  Every PS2 button has a row showing its keyboard key | controller button.
  Select a row, then press the new key or the new controller button (the one
  you press is the one that changes). Backspace while it waits puts back the
  default; Esc cancels. "Reset buttons" puts all of them back. F1, F2, F11 and
  F12 and the controller's Guide / PS button keep their own jobs. (The first
  rows are the on-screen controls of the Android version.)

Game (F2 > Game)
  Skip intro movies    Skips the logo movies and the intro when the game
                       starts (on by default), straight to the menus.
  Controller vibration Lets the game rumble your controller when you hit or
                       get hit, as on the PS2: Xbox and other XInput
                       controllers, and (V9) PlayStation DualShock 4 and
                       DualSense controllers plugged in by USB or paired by
                       Bluetooth (the game's own pause menu has a Vibration
                       switch too).
  Import PCSX2 save    (V9) Copies your Twisted Metal: Head-On saves from a
                       PCSX2 memory card: pick the .ps2 card file (in PCSX2's
                       "memcards" folder), or for a folder memory card its
                       _pcsx2_superblock file. A save of the same name already
                       here is moved to a "backup <date>" folder in saves
                       first. Then load it from the game's own menu.
  Pause on controller loss  Opens the settings and pauses the game when the
                       controller you are playing with disconnects.

CPU cores (F2 > Game, both apply on the next start)
  VU1 thread           The PS2's 3D math unit and the graphics feed run on a
                       CPU core of their own.
  IOP thread           The PS2's I/O processor - the sound driver, disc and
                       memory card - runs on a CPU core of its own, mostly
                       while the game waits for its next frame, instead of on
                       the game's core. Off only to compare.

Sharper font (Vulkan renderer)
  A picture named font_atlas.png in a "textures" folder next to TMHO.exe
  replaces the game's font, so the text can be sharper than the game's own
  512 x 512 one. It is the same 16 x 16 grid of letters, upright, at 2, 4, 8
  or 16 times the size (2048 x 2048 is plenty, even at 4K). Every piece of
  text in the game is drawn from it at the full internal resolution, so the
  letters are sharpest at 2x or 4x. Remove the file to get the game's own
  font back. Graphicslogs.txt notes whether it was used.

Camera (F2 > Game)
  Field of view        How wide the battle camera sees (the game's own view is
                       65 degrees at 4:3, 81 at 16:9).
  Camera distance      How far the chase camera sits behind your car, 50-200%.

Battle HUD (F2 > HUD)
  Edit HUD layout      Shows the radar and the weapon panel over your own game
                       picture: drag a piece to move it, scroll or drag a
                       corner to resize it (arrow keys nudge it, Tab switches
                       piece, R resets the piece, Enter closes). Best opened
                       during a battle.
  Radar size, Weapon panel size   40-250 %.
  Snap to screen edges Moves both pieces right into the corners of the screen,
                       keeping the sizes you picked.
  Reset HUD layout     Puts the HUD back where it starts: right at the screen
                       edges in 16:9, where the game has it in 4:3.

Screenshots: F12 (or F2 > Game > Screenshot > Take) saves the game picture -
without menus or the FPS counter - as a PNG in a "Screenshots" folder next to
TMHO.exe, named by date and time.

Pause in background (F2 > Game) stops the game while its window is not in
front, and carries on when you come back. Mute in background (F2 > Audio)
silences it instead. Minimized, the game keeps running (with those two
options as you set them) but draws nothing, so it barely uses the graphics
card until you bring it back.

Record screenshots (F2 > Game) saves a picture of the game every half second
in Logs\frames (pictures older than 10 minutes are deleted). Handy for bug
reports; leave it off otherwise.

FPS counter: F2 > Game > FPS counter shows "FPS - 60" in the corner: pictures
on screen per second (with in-between frames on, those count too).

Needs Windows 10/11 (64-bit) and a graphics card with Vulkan. Logs go in a
"Logs" folder next to TMHO.exe: performancelogs.txt, Graphicslogs.txt and
console.txt (the game's text output). If something goes wrong, those three
files are what to send. If the game ever freezes, it notes in them where it
got stuck, and if closing takes more than 10 seconds it ends itself, so no
TMHO.exe is left running in the background.

This package contains no game data: you need your own copy of the game.
