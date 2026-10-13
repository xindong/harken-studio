#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;

uniform mat4 mvpMatrix;
uniform mat4 modelMatrix;
uniform mat4 normalMatrix;
uniform vec3 objectColor;
uniform bool useVertexColor;

out vec3 fragNormal;
out vec3 fragPosition;
out vec3 fragColor;

void main()
{
    gl_Position = mvpMatrix * vec4(position, 1.0);
    fragNormal = mat3(normalMatrix) * normal;
    fragPosition = vec3(modelMatrix * vec4(position, 1.0));
    fragColor = useVertexColor ? color : objectColor;
}
