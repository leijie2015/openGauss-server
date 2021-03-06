-- built-in func/view test
-- wlm related
-- set resource_track parameter, so it easy to generate wlm record
select query,username from pg_catalog.gs_wlm_session_info_all;
set resource_track_duration=1;
set resource_track_cost=1;
create table wlmtest1 (a int);
insert into wlmtest1 values(generate_series(1, 1000000));
-- one record
select query,username from pg_catalog.gs_wlm_session_info_all;
reset resource_track_duration;
reset resource_track_cost;
drop table wlmtest1;

create user wlmtest password 'test-1234';
SET SESSION AUTHORIZATION wlmtest password 'test-1234';
set resource_track_duration=1;
set resource_track_cost=1;
create table wlmtest2 (a int);
insert into wlmtest2 values(generate_series(1, 1000000));
-- one record
select query,username from pg_catalog.gs_wlm_session_info_all;
reset resource_track_duration;
reset resource_track_cost;
drop table wlmtest2;

RESET SESSION AUTHORIZATION;
-- two records
select query,username from pg_catalog.gs_wlm_session_info_all order by 1;
-- same as gs_wlm_session_info_all
select query,username from pg_catalog.gs_wlm_session_history order by 1;
select query,username from dbe_perf.statement_complex_history order by 1;
select query,username from dbe_perf.global_statement_complex_history order by 1;

drop user wlmtest;
