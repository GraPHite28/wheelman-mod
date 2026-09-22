#include "ESP.h"
#include "D3DHook.h"

bool WorldToScreen(float worldX, float worldY, float worldZ, float& screenX, float& screenY)
{
    float m[16];
    if (!GetCandidateViewProjMatrix(m)) return false;

    // Row-major, row-vector convention (confirmed empirically: translation
    // sits in the last row, matching plain D3DXMATRIX/HLSL row_major
    // defaults elsewhere in this game) - clip = [worldX,worldY,worldZ,1] * M.
    float clipX = worldX * m[0] + worldY * m[4] + worldZ * m[8]  + m[12];
    float clipY = worldX * m[1] + worldY * m[5] + worldZ * m[9]  + m[13];
    float clipW = worldX * m[3] + worldY * m[7] + worldZ * m[11] + m[15];

    if (clipW < 0.01f) return false; // behind the camera

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(g_viewportWidth);
    screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(g_viewportHeight);
    return true;
}
