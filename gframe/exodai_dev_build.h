#ifndef EXODAI_DEV_BUILD_H
#define EXODAI_DEV_BUILD_H

// Bumped every iteration by Claude when EDOPro-side source is changed.
// Logged to error.log on startup (DataHandler ctor) so its presence
// confirms the running binary regardless of which screens were opened,
// and to the in-game log panel when ML Model is launched.
// See also the matching constant in Windbot/Program.cs — bumped together.
constexpr int EXODAI_DEV_BUILD = 28;

#endif
