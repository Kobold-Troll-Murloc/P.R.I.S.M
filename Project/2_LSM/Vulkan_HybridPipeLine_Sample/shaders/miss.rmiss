#version 460
#extension GL_EXT_ray_tracing : require

struct HitPayload {
    vec3 color;
    float hitT;
};

layout(location = 0) rayPayloadInEXT HitPayload payload;

void main() {
    payload.color = vec3(0.0); // 배경색 (검정)
    payload.hitT = -1.0;       // 맞지 않음을 표시 (-1 또는 아주 큰 값)
}