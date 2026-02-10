#version 460

// Compute가 업데이트한 SSBO 구조체와 동일
struct ObjectData {
    mat4 model;
    vec4 position;
    vec4 velocity;
    vec4 color;
};

// SSBO 바인딩 (Raster Descriptor Set에 SSBO 바인딩 추가 필요!)
// [주의] C++ createGraphicsPipeline()에서 DescriptorSetLayout에 SSBO 바인딩(예: binding 1)을 추가해야 함.
// 지금 당장 DescriptorSetLayout 수정이 어렵다면, 임시로 PushConstant 방식 유지도 가능하나,
// Compute 결과를 보려면 SSBO를 읽어야 합니다.
layout(std140, binding = 1) readonly buffer ObjectBuffer { 
    ObjectData objects[];
} objData;

layout(binding = 0) uniform RasterUBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;

void main() {
    // gl_InstanceIndex는 vkCmdDrawIndexed의 last param(firstInstance)로 전달받은 값
    mat4 modelMatrix = objData.objects[gl_InstanceIndex].model;
    
    gl_Position = ubo.proj * ubo.view * modelMatrix * vec4(inPosition, 1.0);
    
    fragColor = objData.objects[gl_InstanceIndex].color.rgb;
    fragNormal = mat3(modelMatrix) * inNormal;
}