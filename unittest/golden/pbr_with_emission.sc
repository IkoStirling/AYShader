// === material 0 varying.def.sc ===
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;

// === material 0 vs ===
$input a_position, a_normal
$output

#include "common.sh"

uniform vec3 cameraPos;
uniform vec4 emission = vec3(0.0);

void main()
{
    gl_Position = vec4(a_position, 1.0);
}

// === material 0 fs ===
$input v_normal

#include "common.sh"

uniform vec3 cameraPos;
uniform vec4 emission = vec3(0.0);

void main()
{
    vec3 N = normalize(v_normal);
    vec3 result = vec3(0.5);
#ifndef BGFX_VARIANT_USE_EMISSION
    // variant: skipped unless --define BGFX_VARIANT_USE_EMISSION
#else
    result = result + emission;
    gl_FragColor = vec4(result, 1.0);
#endif
}

