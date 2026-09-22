#pragma once

// Same idea as IATHook.cpp but for dinput8.dll!DirectInput8Create: once the
// game's mouse device is created, its GetDeviceState/GetDeviceData get
// hooked so that while Overlay::wantMouseCapture is true, the game sees "no
// mouse input" (buttons up, no movement) instead of fighting our cursor for
// camera/vehicle control. Must be installed before the game creates its
// DirectInput devices, same as InstallD3DHook - i.e. from DllMain, before
// the suspended process is resumed.
bool InstallInputHook();
void RemoveInputHook();
