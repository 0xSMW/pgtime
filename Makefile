# Makefile for pgtime extension

# PostgreSQL Extension Build System (PGXS) setup
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Extension name
EXTENSION = pgtime

# C source files
SRCS = src/gapfill.c src/background.c

# Shared library name (defaults to $(EXTENSION))
MODULES = pgtime

# SQL installation files
# DATA includes the base install script and any upgrade scripts
# Ensure upgrade scripts follow the pattern EXTENSION--OLDVER--NEWVER.sql
DATA = pgtime--0.1.sql $(wildcard pgtime--*--*.sql)

# Documentation files (optional)
DOCS = $(wildcard doc/*.md)

# Configuration file (optional)
# Add timeseries.conf if it contains GUCs to be installed
# CONFIG = timeseries.conf

# Regression tests
REGRESS = basic_usage
REGRESS_OPTS = --inputdir=test --load-extension=pgtime

# Extra clean targets
EXTRA_CLEAN = sql/$(EXTENSION)--*.sql

# Standard PGXS targets (install, uninstall, clean, installcheck) are included via PGXS
# No need to define them manually unless overriding

.PHONY: all install uninstall clean installcheck