#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ngspice/cmtypes.h" // For Digital_t
#include "ngspice/cosim.h"
#include "coroutine.h"
#include "icarus_shim.h"

/* A utility function used to open static libraries, trying an installation
 * directory and different file extenstions.
 */

#if defined (__MINGW32__) || defined (__CYGWIN__) || defined (_MSC_VER)
#include "coroutine_shim.h"

static const char *exts[] = { "", ".so", ".DLL", NULL};
#define CMPFN _stricmp // Ignores case.
#define TESTFN(f) (_access(f, 4) == 0) // Checks for read access.
#define SLIBFILE "DLL"
#define NGSPICELIBDIR "C:\\Spice64\\lib\\ngspice" // Defined by configure.
#define FLAGS 0

#else

#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>

static const char *exts[] = { "", ".so", NULL};
#define CMPFN strcmp
#define TESTFN(f) (access(f, R_OK) == 0)
#define SLIBFILE "shared library"
#define NGSPICELIBDIR "." // Usually defined by configure.
#define FLAGS  (RTLD_GLOBAL | RTLD_NOW)
#endif


static void *cosim_dlopen(const char *fn)
{
    const char **xp;
    int          l1 = strlen(fn), l2;
    void        *handle;
    char         path[1024];

    for (xp = exts; *xp; xp++) {
        l2 = strlen(*xp);
        if (l2 && l1 > l2 && !CMPFN(*xp, fn + l1 - l2))
            continue; // Extension already present
        snprintf(path, sizeof path, "%s%s", fn, *xp);
        handle = dlopen(path, FLAGS);
        if (handle)
            return handle;
        if (TESTFN(path))       // File exists.
            break;
        snprintf(path, sizeof path,  NGSPICELIBDIR "/%s%s", fn, *xp);
        handle = dlopen(path, FLAGS);
        if (handle)
            return handle;
        if (TESTFN(path))
            break;
    }

    fprintf(stderr, "Cannot open " SLIBFILE " %s: %s\n", path, dlerror());
    return NULL;
}

/* Dummy output function. */

static void output(struct co_info *pinfo, unsigned int bit, Digital_t *vp)
{
    printf("Bit %u output %d at %g\n", bit, vp->state, pinfo->vtime);
}

int main(int argc, char **argv)
{
    static Digital_t        val;
    static struct co_info   info;
    void                   *handle;
    void                  (*setup_fn)(struct co_info *);
    double                  ttime = 0;
    int                     i, err, scale = 0, prev, bit, diff;

    *(char ***)&info.sim_argv = argv + 1;
    info.sim_argc = argc - 1;
    info.out_fn = output;
    info.dlopen_fn = cosim_dlopen;
    handle = cosim_dlopen("ivlng");
    if (handle == NULL)
        return 1;
    setup_fn = (void (*)(struct co_info *))dlsym(handle, "Cosim_setup");
    if (handle == NULL) {
        fprintf(stderr, "No symbol Cosim_setup: %s\n", dlerror());
        return 1;
    }        
    (*setup_fn)(&info);
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
