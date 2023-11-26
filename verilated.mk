# -*- Makefile -*-
######################################################################
# DESCRIPTION: Makefile commands for all verilated target files
#
# Copyright 2003-2023 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
######################################################################

AR ?= ar
CXX ?= g++
LINK ?= g++
OBJCACHE ?= 
PERL ?= /usr/bin/perl
PYTHON3 ?= python3

CFG_WITH_CCWARN = no
CFG_WITH_LONGTESTS = no

# Compiler flags to enable profiling
CFG_CXXFLAGS_PROFILE =  -pg
# Select language required to compile (often empty)
CFG_CXXFLAGS_STD = 
# Select newest language (unused by this Makefile, for some test's Makefiles)
CFG_CXXFLAGS_STD_NEWEST = -std=gnu++17
# Compiler flags to use to turn off unused and generated code warnings, such as -Wno-div-by-zero
CFG_CXXFLAGS_NO_UNUSED =  -faligned-new -fcf-protection=none -Wno-bool-operation -Wno-sign-compare -Wno-uninitialized -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-unused-variable -Wno-shadow
# Compiler flags that turn on extra warnings
CFG_CXXFLAGS_WEXTRA =  -Wextra -Wfloat-conversion -Wlogical-op
# Compiler flags that enable coroutine support
CFG_CXXFLAGS_COROUTINES = -fcoroutines
# Linker flags
CFG_LDFLAGS_VERILATED = 
# Linker libraries for multithreading
CFG_LDLIBS_THREADS =  -pthread -lpthread -latomic

######################################################################
# Programs

VERILATOR_COVERAGE = $(PERL) $(VERILATOR_ROOT)/bin/verilator_coverage
VERILATOR_INCLUDER = $(PYTHON3) $(VERILATOR_ROOT)/bin/verilator_includer
#VERILATOR_INCLUDER =  $(PYTHON3) C:\\\\mingw64\\\\share\\\\verilator\\\\bin\\\\verilator_includer
VERILATOR_CCACHE_REPORT = $(PYTHON3) $(VERILATOR_ROOT)/bin/verilator_ccache_report

######################################################################
# Make checks

ifneq ($(words $(CURDIR)),1)
 $(error Unsupported: GNU Make cannot build in directories containing spaces, build elsewhere: '$(CURDIR)')
endif

######################################################################
# OS detection
UNAME_S := $(shell uname -s)

######################################################################
# C Preprocessor flags

# Add -MMD -MP if you're using a recent version of GCC.
VK_CPPFLAGS_ALWAYS += \
		-MMD \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-DVM_COVERAGE=$(VM_COVERAGE) \
		-DVM_SC=$(VM_SC) \
		-DVM_TRACE=$(VM_TRACE) \
		-DVM_TRACE_FST=$(VM_TRACE_FST) \
		-DVM_TRACE_VCD=$(VM_TRACE_VCD) \
		$(CFG_CXXFLAGS_NO_UNUSED) \

ifeq ($(CFG_WITH_CCWARN),yes)  # Local... Else don't burden users
VK_CPPFLAGS_WALL += -Wall $(CFG_CXXFLAGS_WEXTRA) -Werror
endif

CPPFLAGS += -I. $(VK_CPPFLAGS_WALL) $(VK_CPPFLAGS_ALWAYS)

VPATH += ..
VPATH += $(VERILATOR_ROOT)/include
VPATH += $(VERILATOR_ROOT)/include/vltstd

LDFLAGS  += $(CFG_LDFLAGS_VERILATED)

#OPT = -ggdb -DPRINTINITSTR -DDETECTCHANGE
#OPT = -ggdb -DPRINTINITSTR
CPPFLAGS += $(OPT)

CPPFLAGS += $(M32)
LDFLAGS  += $(M32)

# On macOS, specify all weak symbols as dynamic_lookup.
# Otherwise, you get undefined symbol errors.
ifeq ($(UNAME_S),Darwin)
	LDFLAGS += -Wl,-U,__Z15vl_time_stamp64v,-U,__Z13sc_time_stampv
endif

# Allow upper level user makefiles to specify flags they want.
# These aren't ever set by Verilator, so users are free to override them.
CPPFLAGS += $(USER_CPPFLAGS)
LDFLAGS  += $(USER_LDFLAGS)
LDLIBS   += $(USER_LDLIBS)

# Add flags from -CFLAGS and -LDFLAGS on Verilator command line
CPPFLAGS += $(VM_USER_CFLAGS)
LDFLAGS  += $(VM_USER_LDFLAGS)
LDLIBS   += $(VM_USER_LDLIBS)

######################################################################
# Optimization control.

# See also the BENCHMARKING & OPTIMIZATION section of the manual.

# Optimization flags for non performance-critical/rarely executed code.
# No optimization by default, which improves compilation speed.
OPT_SLOW =
# Optimization for performance critical/hot code. Most time is spent in these
# routines. Optimizing by default for improved execution speed.
OPT_FAST = -Os
# Optimization applied to the common run-time library used by verilated models.
# For compatibility this is called OPT_GLOBAL even though it only applies to
# files in the run-time library. Normally there should be no need for the user
# to change this as the library is small, but can have significant speed impact.
OPT_GLOBAL = -Os

#######################################################################
##### Profile builds

ifeq ($(VM_PROFC),1)
  CPPFLAGS += $(CFG_CXXFLAGS_PROFILE)
  LDFLAGS  += $(CFG_CXXFLAGS_PROFILE)
endif

#######################################################################
##### SystemC builds

ifeq ($(VM_SC),1)
  CPPFLAGS += $(SYSTEMC_CXX_FLAGS) $(addprefix -I, $(SYSTEMC_INCLUDE))
  LDFLAGS  += $(SYSTEMC_CXX_FLAGS) $(addprefix -L, $(SYSTEMC_LIBDIR))
  SC_LIBS   = -lsystemc
 ifneq ($(wildcard $(SYSTEMC_LIBDIR)/*numeric_bit*),)
  # Systemc 1.2.1beta
  SC_LIBS   += -lnumeric_bit -lqt
 endif
endif

#######################################################################
##### Threaded builds

CPPFLAGS += $(CFG_CXXFLAGS_STD)
LDLIBS += $(CFG_LDLIBS_THREADS)

ifneq ($(VM_TIMING),0)
 ifneq ($(VM_TIMING),)
  CPPFLAGS += $(CFG_CXXFLAGS_COROUTINES)
 endif
endif


#######################################################################
### Aggregates

VM_FAST += $(VM_CLASSES_FAST) $(VM_SUPPORT_FAST)
VM_SLOW += $(VM_CLASSES_SLOW) $(VM_SUPPORT_SLOW)

#######################################################################
### Overall Objects Linking

VK_FAST_OBJS = $(addsuffix .o, $(VM_FAST))
VK_SLOW_OBJS = $(addsuffix .o, $(VM_SLOW))

VK_USER_OBJS = $(addsuffix .o, $(VM_USER_CLASSES))

# Note VM_GLOBAL_FAST and VM_GLOBAL_SLOW holds the files required from the
# run-time library. In practice everything is actually in VM_GLOBAL_FAST,
# but keeping the distinction for compatibility for now.
VK_GLOBAL_OBJS = $(addsuffix .o, $(VM_GLOBAL_FAST) $(VM_GLOBAL_SLOW))

# Need to re-build if the generated makefile changes, as compiler options might
# have changed.
$(VK_GLOBAL_OBJS): $(VM_PREFIX).mk

ifneq ($(VM_PARALLEL_BUILDS),1)
  # Fast build for small designs: All .cpp files in one fell swoop. This
  # saves total compute, but can be slower if only a little changes. It is
  # also a lot slower for medium to large designs when the speed of the C
  # compiler dominates, which in this mode is not parallelizable.

  VK_OBJS += $(VM_PREFIX)__ALL.o
  $(VM_PREFIX)__ALL.cpp: $(addsuffix .cpp, $(VM_FAST) $(VM_SLOW))
	$(VERILATOR_INCLUDER) -DVL_INCLUDE_OPT=include $^ > $@
  all_cpp: $(VM_PREFIX)__ALL.cpp
else
  # Parallel build: Each .cpp file by itself. This can be somewhat slower for
  # very small designs and examples, but is a lot faster for large designs.

  VK_OBJS += $(VK_FAST_OBJS) $(VK_SLOW_OBJS)
endif

# When archiving just objects (.o), use single $(AR) run
#   1. Make .verilator_deplist.tmp file with list of objects so don't exceed
#      the command line limits when calling $(AR).
#      The approach to write the dependency file is compatible with GNU Make 3,
#      and can be simplified using the file function once GNU Make 4.x becomes
#      the minimum supported version.
# When merging objects (.o) and archives (.a) additionally:
#   1. Extract object files from .a
#   2. Create a new archive from extracted .o and given .o
%.a: | %.verilator_deplist.tmp
	$(info Archive $(AR) -rcs $@ $^)
	$(foreach L, $(filter-out %.a,$^), $(shell echo $L >>$@.verilator_deplist.tmp))
	@if test $(words $(filter %.a,$^)) -eq 0; then \
		$(RM) -f $@; \
		cat $@.verilator_deplist.tmp | xargs $(AR) -rc $@; \
		$(AR) -s $@; \
	else \
		$(RM) -rf $@.tmpdir; \
		for archive in $(filter %.a,$^); do \
			mkdir -p $@.tmpdir/$$(basename $${archive}); \
			cd $@.tmpdir/$$(basename $${archive}); \
			$(AR) -x ../../$${archive}; \
			cd ../..; \
		done; \
		$(RM) -f $@; \
		cat $@.verilator_deplist.tmp | xargs $(AR) -rc $@; \
		$(AR) -rcs $@ $@.tmpdir/*/*.o; \
	fi \
	; $(RM) -rf $@.verilator_deplist.tmp $@.tmpdir

# Truncate the dependency list file used in the %.a target above.
%.verilator_deplist.tmp:
	echo "" > $@

$(VM_PREFIX)__ALL.a: $(VK_OBJS) $(VM_HIER_LIBS)


######################################################################
### Compile rules

ifneq ($(VM_DEFAULT_RULES),0)
# Anything not in $(VK_SLOW_OBJS) or $(VK_GLOBAL_OBJS), including verilated.o
# and user files passed on the Verilator command line use this rule.
# We put OPT_FAST/OPT_SLOW/OPT_GLOBAL before the other flags to
# allow USER_CPPFLAGS to override them
%.o: %.cpp
	$(OBJCACHE) $(CXX) $(OPT_FAST) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(VK_SLOW_OBJS): %.o: %.cpp
	$(OBJCACHE) $(CXX) $(OPT_SLOW) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(VK_GLOBAL_OBJS): %.o: %.cpp
	$(OBJCACHE) $(CXX) $(OPT_GLOBAL) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<
endif

#Default rule embedded in make:
#.cpp.o:
#	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

######################################################################
### ccache report

ifneq ($(findstring ccache-report,$(MAKECMDGOALS)),)
  ifneq ($(OBJCACHE),ccache)
    $(error ccache-report requires OBJCACHE to equal 'ccache')
  endif
  VK_OTHER_GOALS := $(strip $(subst ccache-report,,$(MAKECMDGOALS)))
  ifeq ($(VK_OTHER_GOALS),)
    $(error ccache-report must be used with at least one other explicit target)
  endif

  # Report ccache behaviour for this invocation of make
  VK_CCACHE_LOGDIR := ccache-logs
  VK_CCACHE_REPORT := $(VM_PREFIX)__ccache_report.txt
  # Remove previous logfiles and report
  $(shell rm -rf $(VK_CCACHE_LOGDIR) $(VK_CCACHE_REPORT))

$(VK_CCACHE_LOGDIR):
	mkdir -p $@

$(VK_OBJS): | $(VK_CCACHE_LOGDIR)

$(VK_OBJS): export CCACHE_LOGFILE=$(VK_CCACHE_LOGDIR)/$@.log

$(VK_CCACHE_REPORT): $(VK_OBJS)
	$(VERILATOR_CCACHE_REPORT) -o $@ $(VK_CCACHE_LOGDIR)

.PHONY: ccache-report
ccache-report: $(VK_CCACHE_REPORT)
	@cat $<

# ccache-report runs last
ccache-report: $(VK_OTHER_GOALS)
endif

######################################################################
### Debugging

debug-make::
	@echo
	@echo CXXFLAGS: $(CXXFLAGS)
	@echo CPPFLAGS: $(CPPFLAGS)
	@echo OPT_FAST: $(OPT_FAST)
	@echo OPT_SLOW: $(OPT_SLOW)
	@echo VM_PREFIX:  $(VM_PREFIX)
	@echo VM_PARALLEL_BUILDS:  $(VM_PARALLEL_BUILDS)
	@echo VM_CLASSES_FAST: $(VM_CLASSES_FAST)
	@echo VM_CLASSES_SLOW: $(VM_CLASSES_SLOW)
	@echo VM_SUPPORT_FAST: $(VM_SUPPORT_FAST)
	@echo VM_SUPPORT_SLOW: $(VM_SUPPORT_SLOW)
	@echo VM_GLOBAL_FAST: $(VM_GLOBAL_FAST)
	@echo VM_GLOBAL_SLOW: $(VM_GLOBAL_SLOW)
	@echo VK_OBJS: $(VK_OBJS)
	@echo

######################################################################
### Detect out of date files and rebuild.

DEPS := $(wildcard *.d)
ifneq ($(DEPS),)
include $(DEPS)
endif
