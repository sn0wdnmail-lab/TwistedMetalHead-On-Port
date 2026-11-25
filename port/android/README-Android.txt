Twisted Metal: Head-On - Extra Twisted Edition - Android port (V9.1)
======================================================================

V9.1: text narrowed for 16:9 keeps every stroke (letters no longer lose
lines), Sharp scaling gives every game pixel the same size on screen, a new
Camera tab in the settings (camera distance, camera angle, field of view),
the game starts as it was: 480p, 60 fps, intro movies on and the game's own
camera - and saves load again (they failed with "Load Failed!" before).
The Android build is made and checked on a PC: if something goes wrong on
your phone, the logs (see the end) are what to send.

Needs
  Android 9 or newer on a 64-bit ARM phone or tablet, ideally with a recent
  GPU (Snapdragon 8 Gen 1 or newer, or a similar Exynos / Dimensity / Tensor):
  the game's graphics run on Vulkan. Without what the Vulkan renderer needs,
  the game falls back to the much slower software renderer.
  Your own copy of the game as a disc image (.iso of SCUS-97621).

Install
  1. Copy TMHO-Port-V9.1.apk to the phone (or open it from wherever you keep
     it there) and tap it. Android asks to allow installing apps from that
     source (your file manager or browser): allow it once, then Install.
  2. Copy your .iso to the phone, for example into Download. It stays
     there: the game reads its files straight from it (no 4 GB copy).

First start
  1. Open "TMHO Port". The setup screen opens.
  2. Tap "Allow access" and switch on "Allow access to manage all files" for
     TMHO Port, then go back: the app looks for your .iso on the phone and
     its SD card. With several, "Next disc" steps through them. (Without the
     permission, an .iso in Android/data/io.github.sn0wdnmaillab.tmhoport/
     files is still found.)
  3. Tap "Build game". It only prepares the game program (a few MB); the
     rest is read from the .iso.
  4. Optional: "Build shaders" prepares the GPU shaders now. Skipped, the
     game builds them in the background while you play and saves them.
  5. Tap PLAY.

Playing
  On-screen controls: a D-pad (or analog stick), the four face buttons,
  L1/L2/R1/R2, Select and Start. They hide while you use a controller and
  come back when you touch the screen. The button at the top centre opens
  the settings; so does the phone's Back button.
  Controllers (Bluetooth or USB) work as on PC; F2 > Controls remaps their
  buttons and has the on-screen controls' size, opacity, left-thumb style
  and press vibration.
  The game pauses and goes quiet when you leave the app.
  To quit: settings > Game > Quit game (press Quit twice).

Settings: the game starts at Video > Resolution 480p and Frame rate 60 fps,
the lightest for a phone; try 720p on a fast one. 16:9 widescreen is on from
the start. Camera distance, camera angle and field of view are in the
Camera tab.

PCSX2 saves: copy your .ps2 memory card file into Download, then settings >
Game > Import PCSX2 save.

Files: settings, saves, screenshots, logs and the prepared game program are
in Android/data/io.github.sn0wdnmaillab.tmhoport/files on the phone's
storage (reachable from a PC over USB). If something goes wrong, send the
three files in its Logs folder: performancelogs.txt, Graphicslogs.txt and
console.txt.

Updates: V9.1 installs over V9 and keeps your saves and settings (same
signing key). Later versions install over this one the same way.
