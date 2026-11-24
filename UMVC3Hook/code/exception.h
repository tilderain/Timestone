#pragma once

void Exception_InstallHandler();
void Exception_RemoveHandler();
void Exception_UpdateTestHotkey();

// Runs function(argument) on this thread; a memory fault inside it ends the
// call instead of the game. False when it was cut short that way. Only for
// reading memory that may not be there (another thread's stack, say): the
// stack is not unwound, so nothing the call acquired is released.
bool Exception_RunGuarded(void (*function)(void*), void* argument);
