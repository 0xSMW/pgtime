# Makefile for pgtime extension

# Allow overriding pg_config from the environment
PG_CONFIG ?= pg_config

# Extension name
EXTENSION   = pgtime

# Build one shared library (pgtime.so) from multiple source files
MODULE_big  = pgtime
SRCS        = src/gapfill.c src/background.c

# SQL installation files
DATA = pgtime--0.1.sql $(wildcard pgtime--*--*.sql)

# Documentation files (optional)
DOCS = $(wildcard doc/*.md)

# Regression tests
REGRESS      = basic_usage
REGRESS_OPTS = --inputdir=test --load-extension=pgtime

# Extra clean targets
EXTRA_CLEAN = sql/$(EXTENSION)--*.sql

# Phony convenience targets (PGXS already provides the real rules)
.PHONY: all install uninstall clean installcheck

# ——————————————————————————————————————————————————————
#  Include PostgreSQL’s PGXS **after** all variables above
# ——————————————————————————————————————————————————————
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)