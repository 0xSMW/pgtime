<Plan>
The <code>install</code> step fails because PGXS is told to install documentation
files (<code>DOCS</code>) but the list is empty—<code>install</code> receives only the
destination directory, triggering *usage: install …*.

Solution → only define <code>DOCS</code> when at least one file matches
<code>doc/*.md</code>. We add a conditional block; if no files exist, PGXS
never sees <code>DOCS</code>, and the install phase succeeds.
</Plan>

<file path="Makefile" action="rewrite">
  <change>
    <description>Guard DOCS variable so it is defined only when markdown files exist.</description>
    <content>
===
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

# Documentation files (optional).  Define only if files exist
ifneq ($(wildcard doc/*.md),)
DOCS = $(wildcard doc/*.md)
endif

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
===
    </content>
  </change>
</file>