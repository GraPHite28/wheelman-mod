#pragma once

// Patches Wheelman.exe's own Import Address Table entry for Direct3DCreate9
// so that when the game creates its Direct3D9 object/device (shortly after
// this returns, during its own startup), we intercept the real device and
// hand it to D3DHook::HookDeviceVTable. Must be called before the game's
// main thread reaches that code - i.e. right after injecting into a process
// launched with CREATE_SUSPENDED, before resuming it.
bool InstallD3DHook();
void RemoveD3DHook();
