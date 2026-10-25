/* F1 Challenge 99-02 (ISI, v1.0.2.6): the install layout from the retail disc. Registry values are
 * unknown until the game's real code is readable (grep its strings for "SOFTWARE\\"). */
#ifndef RC_PROFILE_H
#define RC_PROFILE_H

#define PROFILE_EXE        "F1 Challenge 99-02.exe"
#define PROFILE_GUEST_ROOT "C:\\Program Files\\EA SPORTS\\F1 Challenge 99-02"
#define PROFILE_GUEST_EXE  "F1 Challenge 99-02.exe"
#define PROFILE_TITLE      "F1 Challenge 99-02"

#define PROFILE_REGISTRY

/* The game switches its controls to a joystick whenever DirectInput lists one, so none is listed unless RC_PADS asks. */
#define PROFILE_DEFAULT_PADS 0

#endif
