// === compute 0 cs ===
$input
$output

#include "common.sh"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 0) buffer inputs { float data[]; } inputs;
layout(std430, binding = 1) buffer outputs { float data[]; } outputs;

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    (outputs[idx] = (inputs[idx] + 1.0));
}

