#version 440
// 3D stacked traces, after AetherSDR's dss_mesh.vert (GPL-3): a static mesh
// (u across, row back to front, tag) whose heights come from the 3D history
// ring, sampled per vertex. Drawn back to front with opaque coloured
// curtains, so nearer rows hide farther ones without a depth buffer, and a
// neutral ridge ribbon on each. Region = the spectrum area, widget pixels.
//   p0 = (binLo, binHi, head, count)          view bins / ring state
//   p1 = (floorDb, rangeDb, regionW, regionH) floor = noise floor - 6 dB
//   p2 = (ringRows, totalBins, phase, heat?)  phase 0..1 = smooth scroll
// tag: 0 curtain foot, 1 curtain top, 2 / 3 ridge ribbon sides
layout(location = 0) in vec3 mesh;             // (u 0..1, row 0 = newest, tag)
layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_rib;          // -1..1 across the ribbon, 0 curtain
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;
    vec4 p0;
    vec4 p1;
    vec4 p2;
};
layout(binding = 1) uniform sampler2D hist;    // R32F, bins x ringRows
layout(binding = 2) uniform sampler2D palette;
// AetherSDR's geometry + colour constants (DssRenderer.h / SpectrumWidget).
const float kBackWidth = 0.60;                 // back row width / front
const float kDepthSpan = 0.58;                 // baseline rise to the back
const float kRidge = 0.46;                     // front max ridge / plot height
const float kHaze = 0.16;
const float kColorSpanDb = 45.0;               // colour aperture above the floor
const float kGamma = 0.574;                    // "3D Gain" 70
const float kZCurve = 0.70;                    // height lift of the floor band
const float kSpan = 1.0 / kBackWidth;          // rows overhang: no side wedges
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return vec3(0.0, t / 0.25, 1.0);
    if (t < 0.5)  return vec3(0.0, 1.0, 1.0 - (t - 0.25) / 0.25);
    if (t < 0.75) return vec3((t - 0.5) / 0.25, 1.0, 0.0);
    return vec3(1.0, 1.0 - (t - 0.75) / 0.25, 0.0);
}
vec3 pal(float s) {
    float t = pow(clamp(s, 0.0, 1.0), kGamma);
    return p2.w > 0.5 ? heat(t) : texture(palette, vec2(t, 0.5)).rgb;
}
void main() {
    float u = mesh.x;
    int r = int(mesh.y + 0.5);
    float tag = mesh.z;
    int count = int(p0.w), ring = int(p2.x), total = int(p2.y);
    float W = p1.z, H = p1.w;
    float floorDb = p1.x, rangeDb = max(p1.y, 1.0);
    // Depth 0 (front) .. 1 (back); the phase slides every row back smoothly
    // between pushes, so the stack flows instead of stepping.
    float d = (float(r) + p2.z) / float(max(ring - 1, 1));
    // The row covers kSpan x the view's span (the capture holds it); the
    // perspective walks the overhang back in, so frequency lines up with the
    // front row and the far rows still reach the sides.
    float f = 0.5 + (u - 0.5) * kSpan;         // view units, 0..1 on screen
    int binLo = int(p0.x), nv = int(p0.y) - binLo;
    float bf = float(binLo) + f * float(nv);
    bool have = r < count && bf >= 0.0 && bf < float(total);
    float dB = floorDb;
    if (have) {
        // Each mesh column: max of its bins, then AetherSDR's 1-2-1 across
        // neighbouring columns (the rolling-wave texture of the noise).
        int row = ((int(p0.z) - 1 - r) % ring + ring) % ring;
        float colBins = float(nv) * kSpan / 767.0;
        int span = max(1, int(colBins));
        int step = max(1, span / 6);
        float acc = 0.0;
        for (int k = -1; k <= 1; ++k) {
            int b0 = clamp(int(bf + float(k) * colBins), 0, total - 1);
            float m = -1000.0;
            for (int b = b0; b < min(b0 + span, total); b += step)
                m = max(m, texelFetch(hist, ivec2(b, row), 0).r);
            acc += (k == 0 ? 0.5 : 0.25) * m;
        }
        dB = acc;
    }
    float sLin = clamp((dB - floorDb) / rangeDb, 0.0, 1.0);
    float cs = clamp((dB - floorDb) / min(rangeDb, kColorSpanDb), 0.0, 1.0);
    float w = mix(1.0, kBackWidth, clamp(d, 0.0, 1.0));
    float x = (0.5 + (f - 0.5) * w) * W;
    float baseY = mix(1.0, 1.0 - kDepthSpan, clamp(d, 0.0, 1.0)) * H;
    float topY = baseY - pow(sLin, kZCurve) * kRidge * w * H;
    vec3 bg = vec3(12.0, 16.0, 22.0) / 255.0;
    float fade = 1.0 - clamp(d, 0.0, 1.0);
    float y;
    vec3 col;
    float a = have ? 1.0 : 0.0;
    v_rib = 0.0;
    if (tag < 0.5) {                           // curtain foot: dimmer share
        y = baseY;
        col = mix(pal(cs * 0.6), bg, d * kHaze);
    } else if (tag < 1.5) {                    // curtain top: full colour
        y = topY;
        col = mix(pal(cs), bg, d * kHaze);
    } else {                                   // neutral ridge ribbon, 2 px
        float side = tag < 2.5 ? -1.0 : 1.0;
        y = topY + side;
        v_rib = side;
        col = mix(bg, vec3(0.92), 0.45 + 0.55 * fade);
        float flatScale = mix(1.0, 0.42, 1.0 - smoothstep(0.0, 0.035, cs));
        a *= (0.12 + 0.5 * fade) * flatScale;
    }
    v_color = vec4(col, a);
    gl_Position = mvp * vec4(x / W * 2.0 - 1.0, 1.0 - y / H * 2.0, 0.0, 1.0);
}
