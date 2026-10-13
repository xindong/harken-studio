#version 330 core
in vec3 fragColor;

uniform float alpha;

out vec4 outColor;

void main()
{
    outColor = vec4(fragColor, alpha);
}
