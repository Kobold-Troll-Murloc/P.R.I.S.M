#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

// 조명 구조체 정의
struct Light {
    vec3 position;
    float intensity;
    vec3 color;
    int enabled;
};

layout(binding = 2, set = 0) uniform UniformBufferObject {
    mat4 viewInverse;
    mat4 projInverse;
    vec3 cameraPos;
    float padding1;
    Light lights[3];
    int lightCount;
    float padding2[3];
} ubo;

struct Color4 { float r, g, b, a; };

layout(set = 0, binding = 3, std430) buffer InstanceColors {
    Color4 colors[];
};

struct Vertex {
  vec3 pos;
  vec3 normal;
};

layout(buffer_reference, scalar) buffer Vertices { Vertex v[]; };
layout(buffer_reference, scalar) buffer Indices { uint i[]; };

struct ObjDesc {
  uint64_t vertexAddress;
  uint64_t indexAddress;
};

layout(set = 0, binding = 4, scalar) buffer ObjDescBuffer {
  ObjDesc i[];
} objDesc;

// Ray Payloads
layout(location = 0) rayPayloadInEXT vec3 hitValue;
layout(location = 1) rayPayloadEXT bool isShadowed;

hitAttributeEXT vec2 attribs;


vec3 getShadingNormal() {
    uint objId = gl_InstanceCustomIndexEXT;
    ObjDesc desc = objDesc.i[objId];
    Vertices vertices = Vertices(desc.vertexAddress);
    Indices indices = Indices(desc.indexAddress);

    uint ind0 = indices.i[3 * gl_PrimitiveID + 0];
    uint ind1 = indices.i[3 * gl_PrimitiveID + 1];
    uint ind2 = indices.i[3 * gl_PrimitiveID + 2];

    vec3 n0 = vertices.v[ind0].normal;
    vec3 n1 = vertices.v[ind1].normal;
    vec3 n2 = vertices.v[ind2].normal;

    const vec3 barycentrics = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 normal = n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z;
    
    vec3 worldNormal = normalize(vec3(normal * gl_WorldToObjectEXT));
    
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0) {
        worldNormal = -worldNormal;
    }
    return worldNormal;
}

void main() {
    vec3 worldPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 normal = getShadingNormal();
    uint idx = gl_InstanceCustomIndexEXT; 
    vec3 albedo = vec3(colors[idx].r, colors[idx].g, colors[idx].b);
    vec3 finalColor = vec3(0.0);
    vec3 ambient = vec3(0.1) * albedo;

    for(int i = 0; i < ubo.lightCount; i++) {
        Light light = ubo.lights[i];
        
        if(light.enabled == 0) continue;

        vec3 L = normalize(light.position - worldPos);
        float dist = length(light.position - worldPos);
        

        isShadowed = true;
        
        
        traceRayEXT(topLevelAS, 
                    gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 
                    0xFF, 0, 0, 1,
                    worldPos, 0.001, L, dist, 1);

        if (isShadowed) {
            continue;
        }

        float NdotL = max(dot(normal, L), 0.0);
        vec3 diffuse = albedo * light.color * NdotL * light.intensity;

        vec3 viewDir = normalize(ubo.cameraPos - worldPos);
        vec3 halfDir = normalize(L + viewDir);
        float spec = pow(max(dot(normal, halfDir), 0.0), 32.0);
        vec3 specular = vec3(0.3) * spec * light.intensity; 

        finalColor += diffuse + specular;
    }

    hitValue = ambient + finalColor;
}