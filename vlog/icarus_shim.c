/*
 * Main file for the shared library ivlng.so that is used
 * by ngspice's d_cosim code model to connect an instance of
 * an Icarus Verilog simulation (libvvp.so).
 * Licensed on the same terms as Ngspice.
 *
 * Copyright (c) 2024 Giles Atkinson
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#if defined(__MINGW32__) || defined(_MSC_VER)
#include <windows.h> // Windows has a simple co-routine library - "Fibers"

#define dlopen(name, type) LoadLibrary(name)
#define dlsym(handle, name) (void *)GetProcAddress(handle, name)
#define dlclose(handle) FreeLibrary(handle)

char *dlerror(void) // Lifted from dev.c.
{
    static const char errstr_fmt[] =
         "Unable to find message in dlerr(). System code = %lu";
    static char errstr[256];
    LPVOID lpMsgBuf;

    DWORD rc = FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            GetLastError(),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR) &lpMsgBuf,
            0,
            NULL
            );

    if (rc == 0) { /* FormatMessage failed */
        (void) sprintf(errstr, errstr_fmt, (unsigned long) GetLastError());
    } else {
        snprintf(errstr, sizeof errstr, errstr_fmt, lpMsgBuf);
        snprintf(errstr, sizeof errstr, "%s", (char *)lpMsgBuf);
        LocalFree(lpMsgBuf);
    }
    return errstr;
} /* end of function dlerror */
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#include "ngspice/cmtypes.h" // For Digital_t
#include "ngspice/cosim.h"

//#define NGSPICELIBDIR "/usr/local/lib/ngspice"
#define NGSPICELIBDIR "."

/* This header file defines the external interface. It also contains an initial
 * comment that describes how this shared library is used.
 */

#include "icarus_shim.h"

/* The VVP code runs on its own stack, handled by these functions. */

static void cr_init(struct ng_vvp *ctx);
static void cr_yield_to_vvp(struct ng_vvp *ctx);
static void cr_yield_to_spice(struct ng_vvp *ctx);
static void cr_cleanup(struct ng_vvp *ctx);

/* Report fatal errors. */

static void fail(const char *what, int why)
{
    fprintf(stderr, "Icarus shim failed in function %s: %s.\n",
            what, strerror(why));
    abort();
}

static void input(struct co_info *pinfo, unsigned int bit, Digital_t *val)
{
    struct ng_vvp    *ctx = (struct ng_vvp *)pinfo->handle;
    struct ngvp_port *pp;
    int               count, a, b, dirty;

    /* Convert the value. */

    if (val->strength <= HI_IMPEDANCE && val->state != UNKNOWN) {
        a = val->state; // Normal - '0'/'1'.
        b = 0;
    } else if (val->strength == HI_IMPEDANCE) {
        a = 0;          // High impedance - 'z'.
        b = 1;
    } else {
        a = 1;          // Undefined - 'x'.
        b = 1;
    }

    /* Find the port. */

    if (bit >= pinfo->in_count) {
        bit -= pinfo->in_count;
        if (bit >= pinfo->inout_count)
            return;
        pp = ctx->ports + ctx->ins + ctx->outs; // Point at inouts.
        count = ctx->inouts;
    } else {
        pp = ctx->ports;
        count = ctx->ins;
    }

    while (count--) {
        if (pp[count].position <= bit)
            break;
    }
    pp = pp + count;
    bit -= pp->position;

    /* Check and update. */

    dirty = 0;
    bit = pp->bits - bit - 1; // Bit position for big-endian.
    a <<= bit;
    if (a ^ pp->previous.aval) {
        if (a)
            pp->previous.aval |= a;
        else
            pp->previous.aval &= ~(1 << bit);
        dirty = 1;
    }
    b <<= bit;
    if (b ^ pp->previous.bval) {
        if (b)
            pp->previous.bval |= b;
        else
            pp->previous.bval &= ~(1 << bit);
        dirty = 1;
    }

    if (dirty && !(pp->flags & IN_PENDING)) {
        pp->flags |= IN_PENDING;
        ++ctx->in_pending;
    }
}

/* Move the VVP simulation forward. */

static void step(struct co_info *pinfo)
{
    struct ng_vvp *ctx = (struct ng_vvp *)pinfo->handle;
    int            i;

    /* Let VVP run.  It will stop when it has caught up with SPICE time
     * (pinfo->vtime) or produced output.
     */

    cr_yield_to_vvp(ctx);

    /* Check for output. */

    if (ctx->out_pending) {
        struct ngvp_port *pp;
        uint32_t          changed, mask;
        int               limit, i, bit;

        limit = ctx->outs + ctx->inouts;
        for (i = 0, pp = ctx->ports + ctx->ins; i < limit; ++i, ++pp) {
            if (!(pp->flags & OUT_PENDING))
                continue;

            pp->flags &= ~OUT_PENDING;
            changed = (pp->new.aval ^ pp->previous.aval) |
                      (pp->new.bval ^ pp->previous.bval);
            if (changed) {
                bit = pp->position;
                mask = 1 << (pp->bits - 1);
                while (changed) {
                    if (mask & changed) {
                        const Digital_t lv_vals[] =
                            { {ZERO, STRONG}, {ONE, STRONG},
                              {ZERO, HI_IMPEDANCE}, {UNKNOWN, STRONG} };
                        int a, b;

                        a = (pp->new.aval & mask) != 0;
                        b = (pp->new.bval & mask) != 0;
                        a += (b << 1);
                        pinfo->out_fn(pinfo, bit, (Digital_t *)lv_vals + a);
                        changed &= ~mask;
                    }
                    mask >>= 1;
                    ++bit;
                }
                pp->previous.aval = pp->new.aval;
                pp->previous.bval = pp->new.bval;
            }
            if (--ctx->out_pending == 0)
                break;
        }
        if (ctx->out_pending)
            abort();
    }
}

static void cleanup(struct co_info *pinfo)
{
    struct ng_vvp *ctx = (struct ng_vvp *)pinfo->handle;

    if (!ctx)
        return;

    /* Tell VVP to exit. */

    ctx->stop = 1;
    cr_yield_to_vvp(ctx);
    cr_cleanup(ctx);
    free(ctx->ports);
    dlclose(ctx->vvp_handle);
    free(ctx);
    pinfo->handle = NULL;
}

/* Static variable and function for passing context from this library
 * to an instance of ivlng.vpi running in the VVP thread.
 * Get_ng_vvp() is called in the VVP thread and must synchronise.
 * XSPICE initialisation is single-threaded, so a static is OK.
 */

static struct ng_vvp *context;

struct ng_vvp *Get_ng_vvp(void)
{
    return context;
}

/* Thread start function runs the Verilog simulation. */

void *run_vvp(void *arg)
{
    static const char * const  fn_names[] = { VVP_FN_0, VVP_FN_1, VVP_FN_2,
                                              VVP_FN_3, VVP_FN_4, 0 };
    struct co_info            *pinfo = (struct co_info *)arg;
    struct vvp_ptrs            fns;
    void                     **fptr;
    const char                *file;
    struct ng_vvp             *ctx;
    int                        i;

    /* Find the functions to be called in libvvp. */

printf("In run_vvp\n");
    fptr = (void **)&fns;
    for (i = 0; ; ++i, ++fptr) {
        if (!fn_names[i])
            break;
        *fptr = dlsym(context->vvp_handle, fn_names[i]);
        if (!*fptr) {
            fprintf(stderr, "Icarus shim failed to find VVP function: %s.\n",
                    dlerror());
            abort();
        }
    }

    /* Start the simulation. */

    fns.add_module_path(".");
    file = (pinfo->lib_argc >= 3) ? pinfo->lib_argv[2] : NULL; // VVP log file.
    fns.init(file, pinfo->sim_argc, (char **)pinfo->sim_argv);
    fns.no_signals();

    /* The VPI file will usually be /usr/local/lib/ngspice/ivlng.vpi
     * or C:\Spice64\lib\ngspice\ivlng.vpi.
     */

    if (pinfo->lib_argc >= 2 && pinfo->lib_argv[1][0]) // Explicit VPI file.
        file = pinfo->lib_argv[1];
    else
#ifdef STAND_ALONE
        file = "./ivlng";
#else
        file = NGSPICELIBDIR "/ivlng";
#endif

/* Try to load it directly. */
{
    void *handle;

    handle = dlopen("./ivlng.vpi", RTLD_GLOBAL | RTLD_NOW);
    if (handle) {
        printf("Opened VPI file\n");
        dlclose(handle);
    } else {
        printf("Failed to open VPI: %s\n", dlerror());
        fflush(stdout); // ???
    }
 }
    fns.load_module(file);
printf("VVP starting\n");
fflush(stdout); // ???
    fns.run(pinfo->sim_argv[0]);

    /* The simulation has finished.  Do nothing until destroyed. */

printf("VVP done\n");
    ctx = (struct ng_vvp *)pinfo->handle;
    ctx->stop = 1;
    for (;;)
        cr_yield_to_spice(ctx);

    return NULL;
}

/* Entry point to this shared library.  Called by d_cosim. */

void Cosim_setup(struct co_info *pinfo)
{
    char             *file;
    struct ngvp_port *last_port;

    /* It is assumed that there is no parallel access to this function
     * as ngspice initialisation is single-threaded.
     */

printf("Loading libvvp.DLL\n");
fflush(stdout); // ???
    context = calloc(1, sizeof (struct ng_vvp));
    if (!context)
        fail("malloc", errno);
    context->cosim_context = pinfo;

    /* Load libvvp.  It is not statically linked as that would create
     * an unwanted build dependency on Icarus Verilog.
     */

    if (pinfo->lib_argc > 0 && pinfo->lib_argv[0][0]) // Explicit path to VVP?
        file = (char *)pinfo->lib_argv[0];
    else
        file = ".\\libvvp.dll";
    context->vvp_handle = dlopen(file, RTLD_GLOBAL | RTLD_NOW);
    if (!context->vvp_handle) {
        fprintf(stderr, "Icarus shim failed to load VVP library: %s.\n",
                dlerror());
        abort();
    }

    /* Set-up the execution stack for libvvp. */

printf("Calling cr_init()\n");
fflush(stdout); // ???
    cr_init(context);
printf("cr_init() done\n");
fflush(stdout); // ???

    /* Return required values in *pinfo. */

    last_port = context->ports + context->ins - 1;
    pinfo->in_count = context->ins ? last_port->position + last_port->bits : 0;
    last_port += context->outs;
    pinfo->out_count =
        context->outs ? last_port->position + last_port->bits : 0;
    last_port += context->inouts;
    pinfo->inout_count =
        context->inouts ? last_port->position + last_port->bits : 0;
    pinfo->cleanup = cleanup;
    pinfo->step = step;
    pinfo->in_fn = input;
    pinfo->handle = context;
    pinfo->method = Normal;
}

#if defined(__MINGW32__) || defined(_MSC_VER)
static void cr_yield_to_vvp(struct ng_vvp *ctx) {
    SwitchToFiber(ctx->vvp_fiber);
}

static void cr_init(struct ng_vvp *ctx) {
    ctx->spice_fiber = ConvertThreadToFiber(NULL);
printf("cr_init() 1\n");
fflush(stdout); // ???

    /* Start the VVP fiber and wait for it to be ready. */

    ctx->vvp_fiber = CreateFiber(1024*1024, (void (*)(void *))run_vvp,
                                 ctx->cosim_context);
printf("cr_init() 2\n");
fflush(stdout); // ???
    cr_yield_to_vvp(ctx);
printf("cr_init() 3\n");
fflush(stdout); // ???
}

static void cr_cleanup(struct ng_vvp *ctx) {
    DeleteFiber(ctx->vvp_fiber);
}

static void cr_yield_to_spice(struct ng_vvp *ctx) {
    SwitchToFiber(ctx->spice_fiber);
}
#else
/* On a Unix-like OS pthreads are used to give libvvp its own stack,
 * but setcontext() and friends would avoid the overhead of a OS thread.
 */

static void cr_yield_to_vvp(struct ng_vvp *ctx) {
    int err;

    err = pthread_cond_signal(&ctx->vpi_cond);
    if (err)
        fail("pthread_cond_signal (vvp)", err);
    err = pthread_cond_wait(&ctx->spice_cond, &ctx->mutex);
    if (err)
        fail("pthread_cond_wait (spice)", err);
}

static void cr_init(struct ng_vvp *ctx) {
    int err;

    /* Create pthread apparatus. */

    err = pthread_mutex_init(&ctx->mutex, NULL);
    if (err)
        fail("pthread_mutex_init", err);
    err = pthread_cond_init(&ctx->spice_cond, NULL);
    if (!err)
        err = pthread_cond_init(&ctx->vpi_cond, NULL);
    if (err)
        fail("pthread_cond_init", err);

    /* Start the VVP thread and wait for it to be ready. */

    pthread_mutex_lock(&ctx->mutex);
    err = pthread_create(&ctx->thread, NULL, run_vvp, ctx->cosim_context);
    if (err)
        fail("pthread_create", err);
    err = pthread_cond_wait(&ctx->spice_cond, &ctx->mutex);
    if (err)
        fail("pthread_cond_wait", err);
}

static void cr_cleanup(struct ng_vvp *ctx) {
    /* For now, just cancel the VVP thread.
     * It should be in pthread_cond_wait() and will go quickly.
     */

    pthread_cancel(ctx->thread);
    pthread_mutex_unlock(&ctx->mutex);
    pthread_cond_signal(&ctx->vpi_cond); // Make it run
    pthread_join(ctx->thread, NULL);     // Wait for it.
    pthread_cond_destroy(&ctx->spice_cond);
    pthread_cond_destroy(&ctx->vpi_cond);
    pthread_mutex_destroy(&ctx->mutex);
}

static void cr_yield_to_spice(struct ng_vvp *ctx) {
    pthread_cond_signal(&ctx->spice_cond);
    pthread_cond_wait(&ctx->vpi_cond, &ctx->mutex);
}
#endif /* pthread code. */

#ifdef STAND_ALONE

/* Dummy output function. */

static void output(struct co_info *pinfo, unsigned int bit, Digital_t *vp)
{
    printf("Bit %u output %d at %g\n", bit, vp->state, pinfo->vtime);
}

int main(int argc, char **argv)
{
    static Digital_t      val;
    static struct co_info info = {};
    int                   i, err, scale = 0, prev, bit, diff;

    info.sim_argc = argc - 1;   // argv[0] is path to this program.
    *(char ***)&info.sim_argv = argv + 1;
    info.out_fn = output;
    Cosim_setup(&info);
    printf("In main()\n");

    /* Now wait for the simulation to tick.  Count ticks and change the
     * input as simulation proceeds.
     */

#ifdef SCALE
    for (i = 0; i < 1000; ++i) {
        info.vtime += 2e-6;

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
                val.state = data[i][j];
                info.in_fn(&info, j, &val);
            }
        }
        info.vtime += 1e-4;
#endif
        /* Release VVP and wait for it to stall. */

        info.step(&info);
    }
    info.cleanup(&info);
    return 0;
}
#endif
