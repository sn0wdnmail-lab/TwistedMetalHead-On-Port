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
     stutters the first time it appears. The game also adds any new shaders
     to that file itself when you quit. Build them again after a graphics
     driver update.
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
  F2                settings (video, audio, game, HUD, UI colour)
  F1                debugger

To quit, close the window, or use F2 > Game > Quit game (press Quit twice).

Video (F2 > Video) - all of it changes straight away, no restart needed
  Display mode         Windowed, Borderless or Fullscreen.
  Resolution           Windowed: the window size. Fullscreen: the display mode
                       (your monitor's own is the first entry). Borderless: the
                       size the picture is drawn at before it fills the screen.
                       4:3 sizes go from 640 x 480 up to 4K, 16:9 sizes from
                       854 x 480 to 3840 x 2160.
  Aspect ratio         4:3 is the original picture. 16:9 is true widescreen: the
                       game's 3D cameras see more to each side (nothing is
                       stretched), and the HUD, radar and text keep their shape.
                       Pre-rendered movies stay 4:3. Stretch fills the screen.
  Internal resolution  How sharp the GPU draws the game: 1x (640 x 448), 2x or
                       4x. Auto picks 2x for up to 1080p and 4x above.

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

Record screenshots (F2 > Game) saves a picture of the game every half second
in Logs\frames (pictures older than 10 minutes are deleted). Handy for bug
reports; leave it off otherwise.

FPS counter: F2 > Game > FPS counter. The big number is the game's real frame
rate (new pictures per second; 60 is smooth, coloured green, yellow below 50,
red below 25). Underneath: the window's frame rate and the game speed.

Needs Windows 10/11 (64-bit) and a graphics card with Vulkan. Logs go in a
"Logs" folder next to TMHO.exe: performancelogs.txt, Graphicslogs.txt and
console.txt (the game's text output). If something goes wrong, those three
files are what to send. If the game ever freezes, it notes in them where it
got stuck, and if closing takes more than 10 seconds it ends itself, so no
TMHO.exe is left running in the background.

This package contains no game data: you need your own copy of the game.
