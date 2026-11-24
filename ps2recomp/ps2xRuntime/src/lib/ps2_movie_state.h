#pragma once
// Whether the game is showing a pre-rendered movie (MPEG.cpp), for the window's aspect handling.

namespace ps2movie
{
    void notePictureDecoded() noexcept; // game thread, each movie picture
    bool playing() noexcept;            // any thread: a picture was decoded in the last 0.3 s

    // Ends the movie that starts next (or is playing) as if it had reached its end, if it starts
    // within the given time: F2 > Game > Skip intro movies (tmho_game.cpp asks for it when the game
    // starts one of its start-up movies). Game thread.
    void requestSkip(double withinSeconds) noexcept;
    bool skipRequested() noexcept;
    void clearSkipRequest() noexcept;
}
