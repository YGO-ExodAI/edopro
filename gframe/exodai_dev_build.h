#ifndef EXODAI_DEV_BUILD_H
#define EXODAI_DEV_BUILD_H

// Bumped every iteration by Claude when EDOPro-side source is changed.
// Logged to EDOPro's log panel on ML Model launch so the user can confirm
// they're running the latest build. See also the matching constant in
// Windbot/Program.cs — they're bumped together.
constexpr int EXODAI_DEV_BUILD = 21;

#endif
