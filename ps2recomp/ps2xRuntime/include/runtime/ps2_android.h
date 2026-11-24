#ifndef PS2_ANDROID_H
#define PS2_ANDROID_H

// Android version: the app's folders, the activity's state, storage permission and vibration.
// Elsewhere these do nothing (and dataDirectory() is empty).

#include <string>
#include <vector>

namespace ps2android
{
    // True in the Android build.
    bool active() noexcept;

    // The app's own folder on the phone's shared storage (Android/data/<package>/files/), with a
    // trailing '/'. Settings, saves, logs, screenshots, the shader cache and the prepared game
    // program live there; it can be reached from a PC over USB.
    std::string dataDirectory();

    // The app is not in front (another app, the home screen, the notification shade or the lock
    // screen): the game pauses and its sound stops.
    bool inBackground() noexcept;

    // "All files access" (Android 11 and later) or the storage permission (older): needed to find
    // the player's disc image anywhere on the phone. Not needed for an image in dataDirectory().
    bool hasStorageAccess();
    void requestStorageAccess(); // opens the system page where the player allows it

    // A short vibration of the phone (on-screen buttons; the game's rumble with no controller).
    void vibrate(int milliseconds);

    // Folders searched for disc images: the phone's shared storage and any SD card.
    std::vector<std::string> storageRoots();

    // A path as a player finds it in a file manager ("Android/data/.../files/Screenshots" for a
    // folder on the phone's own storage).
    std::string displayPath(const std::string &path);

    // Closes the app's screen (and its entry in the recent apps) before the process ends.
    void finishActivity();
}

#endif
