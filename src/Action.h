// Action.h — turns a mapped ActionId into something the user can see.
//
// Kept separate from WindowManager because not every action touches a window
// directly: task view and show desktop are simulated keystrokes, and mixing
// SendInput with the window-hook code would make both harder to reason about.

#pragma once

#include "Config.h"

#include <string>

// Runs `a`. Always fills `detail` with a short human readable result, including
// the reason when the action could not be performed (no target window, the
// action is disabled in the current mode, ...).
//
// Returns true when something was actually done.
bool runAction(ActionId a, std::string* detail);

// True when `a` is reachable in the given mode. Used by both the executor and
// the API so the UI cannot offer a binding that would never fire.
bool actionAllowedInMode(ActionId a, Mode m);

// Sends an arbitrary recorded chord. Kept here so every synthetic keystroke in
// the program goes through one place (and one extended-key rule).
bool runChord(const KeyChord& k, std::string* detail);

// Starts the program stored with a slot (UTF-8 path). Returns false with a
// reason when the file is gone or the shell refuses it.
bool runLaunch(const std::string& path, std::string* detail);
