#version 460
#extension GL_EXT_ray_tracing : require

// payload location 1번은 그림자 여부를 담습니다.
layout(location = 1) rayPayloadInEXT bool isShadowed;

void main() {
    // 아무것도 부딪히지 않음 = 그림자 없음 (빛 도달)
    isShadowed = false;
}