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

void main()
{
    v_normal = vec3(0.0, 0.0, 1.0);
    v_texcoord0 = vec2(0.0, 0.0);
    vec3 N = normalize(a_normal);
    float NdotL = max(dot(N, vec3(0.0, 1.0, 0.0)), 0.0);
    gl_Position = vec4(a_position, 1.0);
}

// === material 0 fs ===
$input v_normal, v_texcoord0

#include "common.sh"

uniform vec3 cameraPos;

void main()
{
    vec3 N = normalize(v_normal);
    vec3 L = normalize(cameraPos);
    float NdotL = max(dot(N, L), 0.0);
    gl_FragColor = vec4(vec3(NdotL), 1.0);
}

