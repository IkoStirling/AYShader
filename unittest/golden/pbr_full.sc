// === material 0 varying.def.sc ===
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;

// === material 0 vs ===
$input a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0

#include "common.sh"

uniform vec3 cameraPos;
uniform float roughness;
uniform float metallic;
uniform float clearcoatRoughness;
uniform vec3 emission = vec3(0.0, 0.0, 0.0);
uniform vec3 envColor = vec3(0.4, 0.45, 0.5);

void main()
{
    gl_Position = vec4(a_position, 1.0);
}

// === material 0 fs ===
$input v_normal, v_texcoord0

#include "common.sh"

uniform vec3 cameraPos;
uniform float roughness;
uniform float metallic;
uniform float clearcoatRoughness;
uniform vec3 emission = vec3(0.0, 0.0, 0.0);
uniform vec3 envColor = vec3(0.4, 0.45, 0.5);
SAMPLER2D(albedoMap, 0);
SAMPLER2D(normalMap, 1);

void main()
{
    vec3 N = normalize(v_normal);
    vec3 V = normalize(cameraPos);
    vec4 baseColor = texture2D(albedoMap, v_texcoord0);
    vec4 _normalSample = texture2D(normalMap, v_texcoord0);
    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(vec3(0.04), baseColor.rgb, metallic);
    vec3 F = (F0 + (vec3(1.0) - F0) * pow(1.0 - NdotV, 5.0));
    vec3 Fcc = (F0 + (max(vec3(clearcoatRoughness * clearcoatRoughness), vec3(1.0) - F0) - F0) * pow(1.0 - NdotV, 5.0));
    float D = (roughness * roughness / (3.14159265 * pow(NdotV * NdotV * (roughness * roughness - 1.0) + 1.0, 2.0)));
    float G = (NdotV / (NdotV * (1.0 - ((roughness + 1.0) * (roughness + 1.0)) / 8.0) + ((roughness + 1.0) * (roughness + 1.0)) / 8.0));
    vec3 specular = (D * (G * (F / max((4.0 * NdotV), 0.001))));
    vec3 diffuseIBL = (baseColor.rgb * (envColor * ((vec3(1.0) - F) * (1.0 - metallic))));
    vec3 result = (baseColor.rgb + (specular + diffuseIBL));
#ifndef BGFX_VARIANT_USE_EMISSION
    // variant: skipped unless --define BGFX_VARIANT_USE_EMISSION
#else
    (result = (result + emission));
    gl_FragColor = vec4(result, 1.0);
#endif
}

