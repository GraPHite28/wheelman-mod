#pragma once

// Projects a world-space point to screen-space pixel coordinates using the
// vertex shader constant register found to hold the camera's View*Projection
// matrix (see D3DHook.h/.cpp). Returns false if the point is behind the
// camera or no matrix has been captured yet (e.g. before the first frame).
bool WorldToScreen(float worldX, float worldY, float worldZ, float& screenX, float& screenY);
