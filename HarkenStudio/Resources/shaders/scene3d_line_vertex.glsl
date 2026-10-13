#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;

uniform mat4 mvpMatrix;

out vec3 fragColor;

void main()
{
    gl_Position = mvpMatrix * vec4(position, 1.0);
    fragColor = color;
}
