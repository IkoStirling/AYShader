// === material 0 varying.def.sc ===

// === material 0 vs ===
$input
$output

#include "common.sh"

uniform vec4 color = vec4(1.0, 0.0, 0.0, 1.0);

void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}

// === material 0 fs ===
$input

#include "common.sh"

uniform vec4 color = vec4(1.0, 0.0, 0.0, 1.0);

void main()
{
    gl_FragColor = color;
}

