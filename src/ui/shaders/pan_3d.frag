#version 440
layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_rib;
layout(location = 0) out vec4 fragColor;
void main() {
    // Ribbon edges soften analytically (AetherSDR: no line-primitive strobe).
    float cov = v_rib == 0.0 ? 1.0 : 1.0 - smoothstep(0.15, 1.0, abs(v_rib));
    float a = v_color.a * cov;
    fragColor = vec4(v_color.rgb * a, a);
}
