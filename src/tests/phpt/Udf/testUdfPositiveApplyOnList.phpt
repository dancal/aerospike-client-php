--TEST--
Apply UDF on a record containing List.

--FILE--
<?php
include dirname(__FILE__)."/../../astestframework/astest-phpt-loader.inc";
aerospike_phpt_runtest("Udf", "testUdfPositiveApplyOnList");
--EXPECT--
OK
