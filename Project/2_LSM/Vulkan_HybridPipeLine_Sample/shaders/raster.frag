#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // 간단한 라이팅 효과 (Directional Light 느낌)
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float diff = max(dot(normalize(fragNormal), lightDir), 0.1); // Ambient 0.1
    
    vec3 finalColor = fragColor * diff;
    outColor = vec4(finalColor, 1.0);

    // outColor = vec4(1, 0, 0, 1.0);
}