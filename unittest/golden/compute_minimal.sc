// === compute 0 cs ===
$input
$output

#include "common.sh"

layout(local_size_x = 64) in;

buffer counters { int data[]; } counters;

void main()
{
    idx = gl_GlobalInvocationID.x;
    (counters[idx] = (counters[idx] + 1));
}

