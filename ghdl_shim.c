/* Main file to be linked with code generated by GHDL to make a shared library
 * to be loaded by ngspice's d_cosim code model to connect an instance of a
 * VHDL simulation into a Ngspice netlist.
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

/* The GHDL code runs on its own stack, handled by cr_xxx() functions. */

#include "coroutine_shim.h"

#include "ngspice/cmtypes.h" // For Digital_t
#include "ngspice/cosim.h"

/* This header file defines the external (d_cosim) interface. It also contains
 * an initial comment that describes how this shared library is used.
 */

#include "ghdl_shim.h"

extern int ghdl_main(int argc, char **argv); // Not in any header.

/* Report fatal errors. */

static void fail(const char *what, int why)
{
    fprintf(stderr, "Icarus shim failed in function %s: %s.\n",
            what, strerror(why));
    abort();
}

static void input(struct co_info *pinfo, unsigned int bit, Digital_t *val)
{
    struct ng_ghdl    *ctx = (struct ng_ghdl *)pinfo->handle;
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

/* Move the GHDL simulation forward. */

static void step(struct co_info *pinfo)
{
    struct ng_ghdl *ctx = (struct ng_ghdl *)pinfo->handle;
    int            i;

    /* Let GHDL run.  It will stop when it has caught up with SPICE time
     * (pinfo->vtime) or produced output.
     */

    cr_yield_to_sim(&ctx->cr_ctx);

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
    struct ng_ghdl *ctx = (struct ng_ghdl *)pinfo->handle;

    if (!ctx)
        return;

    /* Tell GHDL to exit. */

    ctx->stop = 1;
    cr_yield_to_sim(&ctx->cr_ctx);
    cr_cleanup(&ctx->cr_ctx);
    free(ctx->ports);
    free(ctx);
    pinfo->handle = NULL;
}

/* Static variable and function for passing context from this library
 * to an instance of ivlng.vpi running in the GHDL thread.
 * Get_ng_ghdl() is called in the GHDL thread and must synchronise.
 * XSPICE initialisation is single-threaded, so a static is OK.
 */

static struct ng_ghdl *context;

struct ng_ghdl *Get_ng_ghdl(void)
{
    return context;
}

/* Thread start function runs the VHDL simulation. */

void *run_ghdl(void *arg)
{
    struct co_info   *pinfo = (struct co_info *)arg;
    struct ng_ghdl   *ctx;
    const char       *file;
    const char       *new_argv[pinfo->sim_argc + 2];
    int               new_argc;
    char              vpi_buf[256];

    cr_safety();        // Make safe with signals.

    if (pinfo->sim_argc == 0) {
        new_argc = 1;
        new_argv[0] = "dummy_arg_0";
    } else {
        /* Copy the simulation arguments to the extended array. */

        for (new_argc = 0; new_argc < pinfo->sim_argc; new_argc++)
            new_argv[new_argc] = pinfo->sim_argv[new_argc];
    }

    /* The VPI file will usually be /usr/local/lib/ngspice/ghdllng.vpi
     * or C:\Spice64\lib\ngspice\ghdlng.vpi.
     */

    if (pinfo->lib_argc >= 2 && pinfo->lib_argv[1][0]) // Explicit VPI file.
        file = pinfo->lib_argv[1];
    else
        file = "./ghdlng.vpi";

    /* The GHDL code is passed the VPI module name in a command argument. */

    snprintf(vpi_buf, sizeof vpi_buf, "--vpi=%s", file);
    new_argv[new_argc++] = vpi_buf;
    new_argv[new_argc] = NULL;
    ghdl_main(new_argc, (char **)new_argv);

    /* The simulation has finished.  Do nothing until destroyed. */

    ctx = (struct ng_ghdl *)pinfo->handle;
    ctx->stop = 1;
    for (;;)
        cr_yield_to_spice(&ctx->cr_ctx);

    return NULL;
}

/* Entry point to this shared library.  Called by d_cosim. */

void Cosim_setup(struct co_info *pinfo)
{
    struct ngvp_port *last_port;

    /* It is assumed that there is no parallel access to this function
     * as ngspice initialisation is single-threaded.
     */

    context = calloc(1, sizeof (struct ng_ghdl));
    if (!context)
        fail("malloc", errno);
    context->cosim_context = pinfo;
    pinfo->handle = context;

    /* Set-up the execution stack for the GHDL-generated code and start it. */

    cr_init(&context->cr_ctx, run_ghdl, pinfo);

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
    pinfo->method = Normal;
}
