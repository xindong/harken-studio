#version 330 core
in vec3 fragNormal;
in vec3 fragPosition;
in vec3 fragColor;

uniform vec3 lightDir;
uniform vec3 viewPos;
uniform float alpha;

out vec4 outColor;

void main()
{
    vec3 normal = normalize(fragNormal);
    vec3 lightColor = vec3(1.0, 1.0, 1.0);

    // Ambient
    float ambientStrength = 0.3;
    vec3 ambient = ambientStrength * lightColor;

    // Diffuse
    float diff = max(dot(normal, -lightDir), 0.0);
    vec3 diffuse = diff * lightColor;

    // Specular
    float specularStrength = 0.2;
    vec3 viewDir = normalize(viewPos - fragPosition);
    vec3 reflectDir = reflect(lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = specularStrength * spec * lightColor;

    vec3 result = (ambient + diffuse + specular) * fragColor;
    outColor = vec4(result, alpha);
}
