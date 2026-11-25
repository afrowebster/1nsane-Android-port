/* game-specific hooks (addresses listed in tools/hooks.txt) */
#include <math.h>
#include "win32.h"
float d3d_rt_aspect(void);

void rt_hook(CPU *c, uint32_t at) {
    switch (at) {
    case 0x44e7fa: {
        /* the race camera: 0x42a350(matrix, ..., near, far, horizontal fov (degrees), aspect = 4/3).
           On a wide screen pass a wider horizontal fov with the real aspect: same vertical view (Hor+),
           and the game's own culling and terrain detail cover the whole width */
        float a = d3d_rt_aspect(), asp = grf(c->esp + 0x20), fov = grf(c->esp + 0x1c);
        static int off = -1; if (off < 0) off = getenv("C2_NOWIDEFOV") != NULL;
        if (!off && a > 1.34f && asp > 1.33f && asp < 1.34f && fov > 1 && fov < 170) {
            float h = 2 * atanf(tanf(fov * (float)M_PI / 360) * a / asp) * 180 / (float)M_PI;
            gwf(c->esp + 0x1c, h); gwf(c->esp + 0x20, a);
        }
        break;
    }
    default:
        LOG("unknown hook %x", at);
    }
}

void game_init(void) {}
