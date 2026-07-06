// === material 0 varying.def.sc ===
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_indices   : BLENDINDICES;
vec4 a_weights   : BLENDWEIGHT;

// === material 0 vs ===
$input a_position, a_normal, a_texcoord0, a_indices, a_weights
$output v_normal, v_texcoord0

#include "common.sh"

layout(std140, binding = 0) uniform Skeleton {
    mat4 bones[128];
} Skeleton;

uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec4 baseColor = vec4(1.0, 1.0, 1.0, 1.0);

void main()
{
    v_normal = vec3(0.0, 0.0, 1.0);
    v_texcoord0 = vec2(0.0, 0.0);
    vec4 skinned = ((a_weights.x) * (Skeleton.bones[int(a_indices.x)] * vec4(a_position, 1.0)) + (a_weights.y) * (Skeleton.bones[int(a_indices.y)] * vec4(a_position, 1.0)) + (a_weights.z) * (Skeleton.bones[int(a_indices.z)] * vec4(a_position, 1.0)) + (a_weights.w) * (Skeleton.bones[int(a_indices.w)] * vec4(a_position, 1.0)));
    gl_Position = mul(u_modelViewProj, skinned);
}

// === material 0 fs ===
$input v_normal, v_texcoord0

#include "common.sh"

layout(std140, binding = 0) uniform Skeleton {
    mat4 bones[128];
} Skeleton;

uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec4 baseColor = vec4(1.0, 1.0, 1.0, 1.0);
SAMPLER2D(albedoMap, 0);

void main()
{
    vec4 albedo = (texture2D(albedoMap, v_texcoord0) * baseColor);
    float ndotl = max(dot(normalize(v_normal), -lightDir), 0.05);
    gl_FragColor = vec4((albedo.rgb * (lightColor * ndotl)), albedo.a);
}

