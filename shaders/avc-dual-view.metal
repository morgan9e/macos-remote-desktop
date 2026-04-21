#include <metal_stdlib>
using namespace metal;

inline float rgb_to_y(int r, int g, int b) {
    return float((54 * r + 183 * g + 18 * b) >> 8);
}

inline float rgb_to_u(int r, int g, int b) {
    return float(((-29 * r - 99 * g + 128 * b) >> 8) + 128);
}

inline float rgb_to_v(int r, int g, int b) {
    return float(((128 * r - 116 * g - 12 * b) >> 8) + 128);
}

kernel void createDualView(
    texture2d<float, access::read> srcTexture [[texture(0)]],
    texture2d<float, access::write> mainY [[texture(1)]],
    texture2d<float, access::write> mainUV [[texture(2)]],
    texture2d<float, access::write> auxY [[texture(3)]],
    texture2d<float, access::write> auxUV [[texture(4)]],
    device atomic_uint *needsAuxView [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]],
    uint2 tid [[thread_position_in_threadgroup]])
{
    
    uint2 pos = gid * 2;

    uint width = srcTexture.get_width();
    uint height = srcTexture.get_height();

    if (pos.x >= width || pos.y >= height) {
        return;
    }

    float4 p0 = srcTexture.read(pos);
    float4 p1 = srcTexture.read(pos + uint2(1, 0));
    float4 p2 = srcTexture.read(pos + uint2(0, 1));
    float4 p3 = srcTexture.read(pos + uint2(1, 1));

    int r0 = int(p0.r * 255.0f), g0 = int(p0.g * 255.0f), b0 = int(p0.b * 255.0f);
    int r1 = int(p1.r * 255.0f), g1 = int(p1.g * 255.0f), b1 = int(p1.b * 255.0f);
    int r2 = int(p2.r * 255.0f), g2 = int(p2.g * 255.0f), b2 = int(p2.b * 255.0f);
    int r3 = int(p3.r * 255.0f), g3 = int(p3.g * 255.0f), b3 = int(p3.b * 255.0f);

    float y0 = rgb_to_y(r0, g0, b0);
    float y1 = rgb_to_y(r1, g1, b1);
    float y2 = rgb_to_y(r2, g2, b2);
    float y3 = rgb_to_y(r3, g3, b3);

    float u0 = rgb_to_u(r0, g0, b0);
    float u1 = rgb_to_u(r1, g1, b1);
    float u2 = rgb_to_u(r2, g2, b2);
    float u3 = rgb_to_u(r3, g3, b3);

    float v0 = rgb_to_v(r0, g0, b0);
    float v1 = rgb_to_v(r1, g1, b1);
    float v2 = rgb_to_v(r2, g2, b2);
    float v3 = rgb_to_v(r3, g3, b3);

    float u_avg = (u0 + u1 + u2 + u3) / 4.0f;
    float v_avg = (v0 + v1 + v2 + v3) / 4.0f;

    bool needs_aux = (abs(u_avg - u0) > 30.0f ||
                      abs(u_avg - u1) > 30.0f ||
                      abs(u_avg - u2) > 30.0f ||
                      abs(u_avg - u3) > 30.0f ||
                      abs(v_avg - v0) > 30.0f ||
                      abs(v_avg - v1) > 30.0f ||
                      abs(v_avg - v2) > 30.0f ||
                      abs(v_avg - v3) > 30.0f);

    if (needs_aux) {
        atomic_fetch_or_explicit(needsAuxView, 1, memory_order_relaxed);
    }

    mainY.write(float4(y0 / 255.0f), pos);
    mainY.write(float4(y1 / 255.0f), pos + uint2(1, 0));
    mainY.write(float4(y2 / 255.0f), pos + uint2(0, 1));
    mainY.write(float4(y3 / 255.0f), pos + uint2(1, 1));

    mainUV.write(float4(u_avg / 255.0f, v_avg / 255.0f, 0, 0), gid);

    uint tw_half = width / 2;

    auxY.write(float4(u1 / 255.0f), uint2(gid.x, pos.y));
    auxY.write(float4(v1 / 255.0f), uint2(gid.x + tw_half, pos.y));
    auxY.write(float4(u3 / 255.0f), uint2(gid.x, pos.y + 1));
    auxY.write(float4(v3 / 255.0f), uint2(gid.x + tw_half, pos.y + 1));

    auxUV.write(float4(u2 / 255.0f, v2 / 255.0f, 0, 0), gid);
}

kernel void createMainViewOnly(
    texture2d<float, access::read> srcTexture [[texture(0)]],
    texture2d<float, access::write> mainY [[texture(1)]],
    texture2d<float, access::write> mainUV [[texture(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint2 pos = gid * 2;

    uint width = srcTexture.get_width();
    uint height = srcTexture.get_height();

    if (pos.x >= width || pos.y >= height) {
        return;
    }

    float4 p0 = srcTexture.read(pos);
    float4 p1 = srcTexture.read(pos + uint2(1, 0));
    float4 p2 = srcTexture.read(pos + uint2(0, 1));
    float4 p3 = srcTexture.read(pos + uint2(1, 1));

    int r0 = int(p0.r * 255.0f), g0 = int(p0.g * 255.0f), b0 = int(p0.b * 255.0f);
    int r1 = int(p1.r * 255.0f), g1 = int(p1.g * 255.0f), b1 = int(p1.b * 255.0f);
    int r2 = int(p2.r * 255.0f), g2 = int(p2.g * 255.0f), b2 = int(p2.b * 255.0f);
    int r3 = int(p3.r * 255.0f), g3 = int(p3.g * 255.0f), b3 = int(p3.b * 255.0f);

    float y0 = rgb_to_y(r0, g0, b0);
    float y1 = rgb_to_y(r1, g1, b1);
    float y2 = rgb_to_y(r2, g2, b2);
    float y3 = rgb_to_y(r3, g3, b3);

    float u_avg = (rgb_to_u(r0, g0, b0) + rgb_to_u(r1, g1, b1) +
                   rgb_to_u(r2, g2, b2) + rgb_to_u(r3, g3, b3)) / 4.0f;
    float v_avg = (rgb_to_v(r0, g0, b0) + rgb_to_v(r1, g1, b1) +
                   rgb_to_v(r2, g2, b2) + rgb_to_v(r3, g3, b3)) / 4.0f;

    mainY.write(float4(y0 / 255.0f), pos);
    mainY.write(float4(y1 / 255.0f), pos + uint2(1, 0));
    mainY.write(float4(y2 / 255.0f), pos + uint2(0, 1));
    mainY.write(float4(y3 / 255.0f), pos + uint2(1, 1));

    mainUV.write(float4(u_avg / 255.0f, v_avg / 255.0f, 0, 0), gid);
}