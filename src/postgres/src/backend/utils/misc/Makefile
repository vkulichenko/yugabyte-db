#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/misc
#
# IDENTIFICATION
#    src/backend/utils/misc/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/misc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = backend_random.o guc.o help_config.o pg_config.o pg_controldata.o \
       pg_rusage.o ps_status.o queryenvironment.o rls.o sampling.o \
       superuser.o timeout.o tzparser.o \
       pg_yb_utils.o yb_safe_funcs_for_pushdown.o

# This location might depend on the installation directories. Therefore
# we can't substitute it into pg_config.h.
ifdef krb_srvtab
override CPPFLAGS += -DPG_KRB_SRVTAB='"$(krb_srvtab)"'
endif

include $(top_srcdir)/src/backend/common.mk

# guc-file is compiled as part of guc
guc.o: guc-file.c

# Note: guc-file.c is not deleted by 'make clean',
# since we want to ship it in distribution tarballs.
clean:
	@rm -f lex.yy.c
