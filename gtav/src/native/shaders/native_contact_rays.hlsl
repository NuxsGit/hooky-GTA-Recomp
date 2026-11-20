// Native contact occlusion: short rays in (screen x, screen y, device depth).
// This visibility multiplier is blended into the scene's shadow-mask alpha only.
// No light direction, world-space intersections, off-screen geometry or temporal history.
struct ContactPush {
    uint DepthIndex;
    uint Width;
    uint Height;
    uint Quality;
    float Strength;
    uint Reversed;
};
[[vk::push_constant]] ConstantBuffer<ContactPush> g_Push;
Texture2D<float4> g_Textures[] : register(t0, space0);

static const float2 kDirections[8] = {
    float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
    float2(0.70710678, 0.70710678), float2(-0.70710678, -0.70710678),
    float2(0.70710678, -0.70710678), float2(-0.70710678, 0.70710678),
};

// For a perspective camera, distance from the far plane in device depth is proportional
// to reciprocal distance minus a constant. It's not linear Z. Use only local, relative
// differences: treating this buffer as metres produces large halos around silhouettes.
float Proximity(int2 pixel, int2 size) {
    if (any(pixel < 0) || any(pixel >= size)) return 0.0;
    float z = g_Textures[g_Push.DepthIndex].Load(int3(pixel, 0)).r;
    if (!(z >= 0.0 && z <= 1.0)) return 0.0;
    return g_Push.Reversed != 0 ? z : (1.0 - z);
}

float Slope(float a, float center, float b) {
    // Prefer the side that's continuous with the receiver (avoid the depth discontinuity).
    float lo = a > 0.0 ? center - a : center - b;
    float hi = b > 0.0 ? b - center : lo;
    if (a <= 0.0 && b > 0.0) lo = hi;
    if (a <= 0.0 && b <= 0.0) return 0.0;
    return abs(lo) < abs(hi) ? lo : hi;
}

float4 PSContactRays(float2 uv : TEXCOORD0) : SV_Target {
    if (g_Push.Quality == 0 || g_Push.Quality > 3 || !(g_Push.Strength > 0.0)) return 1.0;
    const int2 size = int2(g_Push.Width, g_Push.Height);
    const int2 pixel = clamp(int2(uv * float2(size)), 0, size - 1);
    const float center = Proximity(pixel, size);
    const float fade = smoothstep(0.0005, 0.003, center);
    if (fade <= 0.0) return 1.0;  // far-plane / sky / invalid depth

    const float2 slope = float2(
        Slope(Proximity(pixel - int2(1, 0), size), center, Proximity(pixel + int2(1, 0), size)),
        Slope(Proximity(pixel - int2(0, 1), size), center, Proximity(pixel + int2(0, 1), size)));
    const int rays = g_Push.Quality == 3 ? 8 : 4;
    const int steps = g_Push.Quality == 1 ? 3 : 5;
    const float pixelScale = max(0.5, float(size.y) / 720.0);
    float occlusion = 0.0;
    [loop] for (int ray = 0; ray < rays; ++ray) {
        float hit = 0.0;
        [loop] for (int step = 0; step < steps; ++step) {
            float t = float(step) / float(steps - 1);
            int2 offset = int2(round(kDirections[ray] * (lerp(2.0, 10.0, t) * pixelScale)));
            float sampleZ = Proximity(pixel + offset, size);
            if (sampleZ <= 0.0 || all(offset == 0)) continue;
            // Start on the estimated receiver plane with a small bias; march towards the
            // camera over a bounded distance and test a finite depth interval at each step.
            float rayZ = center + dot(slope, float2(offset)) + center * lerp(0.012, 0.04, t);
            float gap = (sampleZ - rayZ) / center;
            float weight = smoothstep(0.0, 0.035, gap) * (1.0 - smoothstep(0.12, 0.24, gap));
            hit = max(hit, weight * lerp(1.0, 0.25, t));
        }
        occlusion += hit;
    }
    const float visibility = 1.0 - 0.65 * saturate(g_Push.Strength) * fade * (occlusion / float(rays));
    return float4(1.0, 1.0, 1.0, visibility);
}
