#version 440
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(v_color.rgb * v_color.a, v_color.a); }
