#pragma once
// Twisted Metal: Head-On specific settings support (tmho_game.cpp).

namespace tmho
{
    // The battle camera's horizontal field of view in degrees with the game's own setting, for
    // the current aspect ratio (65 at 4:3, 81 at 16:9).
    float defaultHorizontalFov();
    // The horizontal scale the battle camera should use, given the game's (already widescreen
    // adjusted) value: unchanged unless a field of view is set in F2 > Game.
    float gameplayHorizontalScale(float scale);
    // Call after F2 settings change (any thread): updates the game's viewport tables in RAM.
    void settingsChanged();

    // ---- Battle HUD layout (F2 > HUD) ----
    // The 1P battle HUD comes in two pieces: the radar (top left, with the cars-left count and the
    // target's name) and the weapon panel (bottom right: special weapon, ammo, health and turbo).
    enum class HudPiece
    {
        Radar,
        Weapon,
        Count
    };
    // Where a piece sits with its default layout and size, as a box in the game picture: x and
    // width in fractions of the picture's width, y and height in fractions of its height.
    struct HudBox
    {
        float x, y, width, height;
    };
    HudBox hudDefaultBox(HudPiece piece);
    // The same box with the layout in the settings (offset and size) applied.
    HudBox hudBox(HudPiece piece);
    // True when the game drew the battle HUD within the last `seconds` (window thread: is there a
    // live HUD behind the HUD editor?).
    bool hudShownRecently(double seconds);
    // Call after the HUD layout settings change (any thread): the game thread moves the HUD's
    // models at its next HUD text.
    void hudLayoutChanged();
}
