MODULE_big = hbase_fdw
OBJS = hbase_fdw.o hbase_thrift.o

EXTENSION = hbase_fdw
DATA = hbase_fdw--1.0.sql
PGFILEDESC = "hbase_fdw - foreign data wrapper for HBase"

# regression test needs a running HBase thrift server with test data,
# see README (make installcheck)
REGRESS = hbase_fdw hbase_batch hbase_write

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hbase_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
