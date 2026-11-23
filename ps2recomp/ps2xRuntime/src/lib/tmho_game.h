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
}
