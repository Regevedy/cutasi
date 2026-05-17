#pragma once
// Compatibility wrapper for the official ScriptHookV SDK layouts.
// Some SDK packages provide inc/main.h but do not include sample script.h.
#include "main.h"

#ifndef WAIT
#define WAIT(ms) scriptWait(ms)
#endif
