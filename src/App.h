#pragma once

#include <string>

// Comic Reader: the library and the reader on the playfield, the book's cover
// on the backglass, the page on the DMD. Returns when the user exits.
// kRestart: an update was installed; start the app again.
constexpr int kRestart = 75;
int runApp(const std::string& appDir);
