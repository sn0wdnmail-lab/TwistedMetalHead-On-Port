#pragma once
// Whether the game is showing a pre-rendered movie (MPEG.cpp), for the window's aspect handling.

namespace ps2movie
{
    void notePictureDecoded() noexcept; // game thread, each movie picture
    bool playing() noexcept;            // any thread: a picture was decoded in the last 0.3 s
}
