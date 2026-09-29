MODULES = pg_textsearch_test
EXTENSION = pg_textsearch_test
DATA = pg_textsearch_test--1.0.sql
PG_CPPFLAGS = -I../../../src

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
