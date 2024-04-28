#include <stdio.h>
#include <stdint.h>

#if defined(__MINGW32__) || defined(_MSC_VER)
#include <windows.h> // Windows has a simple co-routine library - "Fibers"
#else
#include <pthread.h>
#endif

#include "ngspice/cmtypes.h" // For Digital_t
#include "ngspice/cosim.h"
#include "icarus_shim.h"

/* Dummy output function. */

static void output(struct co_info *pinfo, unsigned int bit, Digital_t *vp)
{
    printf("Bit %u output %d at %g\n", bit, vp->state, pinfo->vtime);
}

int main(int argc, char **argv)
{
    static Digital_t      val;
    static struct co_info info;
    double                ttime = 0;
    int                   i, err, scale = 0, prev, bit, diff;

    *(char ***)&info.sim_argv = argv + 1;
    info.sim_argc = argc - 1;
    info.out_fn = output;
    Cosim_setup(&info);
    printf("In main()\n");

    /* Now wait for the simulation to tick.  Count ticks and change the
     * input as simulation proceeds.
     */

#ifdef SCALE
    for (i = 0; i < 1000; ++i) {
        ttime += 2e-6;

        /* Assume a single 6-bit input. */

        if ((i % 100) == 0) {
            prev = scale++;
            diff = prev ^ scale;
            printf("New scale %d\n", scale);
            for (bit = 0; diff; ++bit) {
                if (diff & 1) {
                    val.state = !!(scale & (1 << bit));
                    info.in_fn(&info, 5 - bit, &val); // wire [5 : 0] scale
                }
                diff >>= 1;
            }
        }
#else // 555 for now
    static unsigned char data[][3] = {
        {0, 0, 1}, // Out of reset
        {1, 0, 1}, // Trigger
        {0, 0, 1}, // Trigger off
        {0, 1, 1}, // Threshold
        {0, 0, 1}, // Threshold off
        {1, 0, 1}, // Trigger
        {0, 0, 1}, // Trigger off
        {0, 0, 0}, // Reset
        {1, 0, 0}, // Trigger/Reset
    };
#if 0
    static const char *nets[] = {
        "VL555.Trigger", "VL555.Threshold", "VL555.Reset",
        "VL555.Q", "VL555.Qbar", "VL555.ireset", "VL555.go", NULL};

    extern void dbg_register_name(void *ref);
    extern void *vpi_handle_by_name(const char *, void *);
    
    for (i = 0; nets[i]; ++i)
        dbg_register_name(vpi_handle_by_name(nets[i], NULL));
#endif
    for (i = 0; i < 2 * (sizeof data / 3); ++i) {
        int j;

        if (i & 1) {
            for (j = 0; j < 3; j++) {
                val.state = data[i / 2][j];
                info.in_fn(&info, j, &val);
            }
        }
        ttime += 1e-4;
#endif
        /* Release VVP and wait for it to stall. */

        do {
            info.vtime = ttime;
            info.step(&info);
        } while (info.vtime != ttime);
    }
    info.cleanup(&info);
    return 0;
}
