\echo Use "CREATE EXTENSION hbase_fdw" to load this file. \quit

CREATE FUNCTION hbase_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION hbase_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER hbase_fdw
  HANDLER hbase_fdw_handler
  VALIDATOR hbase_fdw_validator;
