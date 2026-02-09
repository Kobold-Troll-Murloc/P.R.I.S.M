#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitValue;
layout(location = 1) rayPayloadInEXT bool isShadowed;

void main()
{
    // 그림자 레이가 여기까지 왔다는 건, 가리는 물체가 없다는 뜻
    isShadowed = false;
}
