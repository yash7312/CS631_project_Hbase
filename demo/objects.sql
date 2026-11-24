-- the demo database: extension, server and one foreign table over HBase table emp
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS hbase_fdw CASCADE;
CREATE EXTENSION hbase_fdw;
RESET client_min_messages;

CREATE SERVER hb FOREIGN DATA WRAPPER hbase_fdw
  OPTIONS (host 'localhost', port '9090');

CREATE FOREIGN TABLE emp (
  id     text OPTIONS (rowkey 'true') COLLATE "C",
  name   text OPTIONS (column 'info:name') NOT NULL,
  dept   text OPTIONS (column 'info:dept'),
  salary int  OPTIONS (column 'pay:salary')
) SERVER hb OPTIONS (table 'emp', rows '10000');
