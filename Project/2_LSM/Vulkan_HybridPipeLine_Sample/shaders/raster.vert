#version 450

layout(binding = 0) uniform RasterUBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 model;
    vec3 color;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    fragColor = push.color;
    // 간단한 노멀 변환 (실제로는 inverse transpose 필요할 수 있음)
    fragNormal = mat3(push.model) * inNormal; 
}