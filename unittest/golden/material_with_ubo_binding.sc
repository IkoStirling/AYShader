// === material 0 varying.def.sc ===

// === material 0 vs ===
$input
$output

#include "common.sh"

layout(std140, binding = 0) uniform Camera {
    vec3 position;
    float fov;
} Camera;


void main()
{
    p = Camera.position;
    gl_Position = vec4(p, 1.0);
}

// === material 0 fs ===
$input

#include "common.sh"

layout(std140, binding = 0) uniform Camera {
    vec3 position;
    float fov;
} Camera;


void main()
{
    gl_FragColor = vec4(Camera.fov, 0.0, 0.0, 1.0);
}

