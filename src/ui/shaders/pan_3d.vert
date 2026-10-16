#version 440
// 3D stacked traces: a static mesh (u across, row back to front, tag) whose
// heights come from the 3D history ring, sampled here per vertex. Drawn back
// to front with opaque curtains, so nearer rows hide farther ones without a
// depth buffer. Region = the spectrum area, in widget pixels.
//   p0 = (binLo, binHi, head, count)          view bins / ring state
//   p1 = (dbMin, dbMax, regionW, regionH)
//   p2 = (ringRows, meshRows, phase, heat?)   phase 0..1 = smooth scroll
// tag: 0 curtain base, 1 curtain top, 2 ridge top, 3 ridge bottom
layout(location = 0) in vec3 mesh;             // (u 0..1, row 0 = newest, tag)
layout(location = 0) out vec4 v_color;
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;
    vec4 p0;
    vec4 p1;
    vec4 p2;
};
layout(binding = 1) uniform sampler2D hist;    // R32F, bins x ringRows
layout(binding = 2) uniform sampler2D palette;
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return vec3(0.0, t / 0.25, 1.0);
    if (t < 0.5)  return vec3(0.0, 1.0, 1.0 - (t - 0.25) / 0.25);
    if (t < 0.75) return vec3((t - 0.5) / 0.25, 1.0, 0.0);
    return vec3(1.0, 1.0 - (t - 0.75) / 0.25, 0.0);
}
void main() {
    float u = mesh.x;
    int r = int(mesh.y + 0.5);
    float tag = mesh.z;
    int count = int(p0.w), ring = int(p2.x), nRows = int(p2.y);
    float W = p1.z, H = p1.w;
    // Depth 0 (front) .. 1 (back); the phase slides every row back smoothly
    // between pushes, so the stack flows instead of stepping.
    float d = (float(r) + p2.z) / float(max(nRows - 1, 1));
    float widthScale = 1.0 - 0.36 * d;
    float ampScale = 1.0 - 0.45 * d;
    float yFront = H - 3.0;
    float yBack = H * 0.30;
    float baseY = yFront - (yFront - yBack) * pow(d, 0.85);
    float amp = (yFront - 18.0) * 0.80 * ampScale;
    // Level: max of this mesh column's bins (<= 8 fetches).
    float level = 0.0;
    bool have = r < count;
    if (have) {
        int binLo = int(p0.x), nv = int(p0.y) - binLo;
        int row = ((int(p0.z) - 1 - r) % ring + ring) % ring;
        int b0 = binLo + int(u * float(nv - 1));
        int span = max(1, nv / 512);
        int step = max(1, span / 8);
        float m = -1000.0;
        for (int b = b0; b < min(b0 + span, binLo + nv); b += step)
            m = max(m, texelFetch(hist, ivec2(b, row), 0).r);
        level = clamp((m - p1.x) / (p1.y - p1.x), 0.0, 1.0);
    }
    float x = W * 0.5 + (u - 0.5) * W * widthScale;
    float topY = baseY - level * amp;
    float y = tag < 0.5 ? baseY : tag < 2.5 ? topY : topY + 1.6;
    if (!have) y = baseY;                      // not filled yet: flat, hidden
    vec3 c = p2.w > 0.5 ? heat(level) : texture(palette, vec2(level, 0.5)).rgb;
    vec3 bg = vec3(12.0, 16.0, 22.0) / 255.0;
    float fog = 0.55 * clamp(d, 0.0, 1.0);
    vec3 col;
    if (tag < 0.5)      col = bg;                              // curtain foot
    else if (tag < 1.5) col = mix(c * 0.45, bg, 0.25);         // curtain top
    else                col = mix(c, vec3(1.0), 0.15);         // ridge line
    col = mix(col, bg, fog);
    v_color = vec4(have ? col : bg, have ? 1.0 : 0.0);
    gl_Position = mvp * vec4(x / W * 2.0 - 1.0, 1.0 - y / H * 2.0, 0.0, 1.0);
}
