#version 440
// KE9NS fill under the trace: colour keyed to the dB AXIS (bands run
// horizontally), gamma-compressed; transparent above the trace.
//   p0 = (regionW, regionH, gamma, unused)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;
    vec4 p0;
    vec4 p1;
    vec4 p2;
};
layout(binding = 1) uniform sampler2D cols;    // R32F, one 0..1 level per pixel column
layout(binding = 2) uniform sampler2D palette;
void main() {
    int x = int(floor(v_uv.x * p0.x));
    float h = p0.y;
    float y = floor(v_uv.y * h);
    float level = texelFetch(cols, ivec2(x, 0), 0).r;
    float colY = floor(h * (1.0 - level));
    if (y < colY) { fragColor = vec4(0.0); return; }
    float t = 1.0 - y / max(h - 1.0, 1.0);
    fragColor = vec4(texture(palette, vec2(pow(t, p0.z), 0.5)).rgb, 1.0);
}
