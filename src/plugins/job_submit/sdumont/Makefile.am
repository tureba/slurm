# Makefile for job_submit/sdumont plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = job_submit_sdumont.la

# Job submit sdumont plugin.
job_submit_sdumont_la_SOURCES = job_submit_sdumont.c
job_submit_sdumont_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS) -lslurmdb
