// Spatial AMD FidelityFX Super Resolution 1.0. FSR runs on the SDR guest front buffer;
// its HUD is already composited, but the host overlays are drawn afterwards at output resolution.
// Bindings and 24-byte push constant range match the native blit pipeline layout.
struct FsrPush {
    uint TextureIndex;
    uint SamplerIndex;
    uint2 InputSize;
    uint2 OutputSize;
};
[[vk::push_constant]] ConstantBuffer<FsrPush> g_Fsr;
Texture2D<float4> g_FsrTextures[] : register(t0, space0);
SamplerState g_FsrSamplers[] : register(s0, space3);

#define A_GPU 1
#define A_HLSL 1
#include "fsr1/ffx_a.h"

#if GTAV_FSR_EASU
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return g_FsrTextures[g_Fsr.TextureIndex].GatherRed(g_FsrSamplers[g_Fsr.SamplerIndex], p); }
AF4 FsrEasuGF(AF2 p) { return g_FsrTextures[g_Fsr.TextureIndex].GatherGreen(g_FsrSamplers[g_Fsr.SamplerIndex], p); }
AF4 FsrEasuBF(AF2 p) { return g_FsrTextures[g_Fsr.TextureIndex].GatherBlue(g_FsrSamplers[g_Fsr.SamplerIndex], p); }
#else
#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
AF4 FsrRcasLoadF(ASU2 p) {
    // RCAS reads a one-pixel neighbourhood. Clamp signed coordinates to avoid a dark rim from
    // out-of-bounds Load at any of the four edges (including the first row/column).
    return g_FsrTextures[g_Fsr.TextureIndex].Load(int3(clamp(p, 0, int2(g_Fsr.InputSize) - 1), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#include "fsr1/ffx_fsr1.h"

#if GTAV_FSR_EASU
float4 PSFsrEasu(float4 position : SV_Position) : SV_Target {
    uint4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, g_Fsr.InputSize.x, g_Fsr.InputSize.y,
               g_Fsr.InputSize.x, g_Fsr.InputSize.y, g_Fsr.OutputSize.x, g_Fsr.OutputSize.y);
    float3 color;
    FsrEasuF(color, uint2(position.xy), con0, con1, con2, con3);  // viewport starts at (0, 0)
    return float4(color, 1.0);
}
#else
float4 PSFsrRcas(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint4 con;
    FsrRcasCon(con, 1.0);  // One stop below maximum sharpening; the guest HUD is part of the input.
    // Always a 1:1 copy. UV is relative to the game viewport, even when letterboxed.
    uint2 pixel = min(uint2(uv * float2(g_Fsr.InputSize)), g_Fsr.InputSize - 1u);
    float3 color;
    FsrRcasF(color.r, color.g, color.b, pixel, con);
    return float4(color, 1.0);
}
#endif
