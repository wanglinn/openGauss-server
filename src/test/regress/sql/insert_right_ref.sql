create database rightref with dbcompatibility 'B';
\c rightref

-- test fields order
create table test_order_t(n1 int default 100, n2 int default 100, s int);
insert into test_order_t values(1000, 1000, n1 + n2);
insert into test_order_t(s, n1, n2) values(n1 + n2, 300,  300);
select * from test_order_t;
drop table test_order_t;

-- test non-idempotent function
create table non_idempotent_t(c1 float, c2 float, c3 float);
insert into non_idempotent_t values(random(), c1, c1);
select c1 = c2 as f1, c1 = c3 as f2 from non_idempotent_t;
drop table non_idempotent_t;

-- test auto increment
create table auto_increment_t(n int, c1 int primary key auto_increment, c2 int, c3 int);
insert into auto_increment_t values(1, c1, c1, c1);
insert into auto_increment_t values(2, 0, c1, c1);
insert into auto_increment_t values(3, 0, c1, c1);
insert into auto_increment_t values(4, -1, c1, c1);
insert into auto_increment_t(n, c2, c3, c1) values(5, c1, c1, 1000);
insert into auto_increment_t values(5, c1, c1, c1);
select * from auto_increment_t order by n;
drop table auto_increment_t;

-- test series
create table test_series_t(c1 int, c2 int, c3 int);
insert into test_series_t values(c2 + 10, generate_series(1, 10), c2 * 2);
select * from test_series_t;
drop table test_series_t;

-- test upsert
-- 1
create table upser(c1 int, c2 int, c3 int);
create unique index idx_upser_c1 on upser(c1);
insert into upser values (1, 10, 10), (2, 10, 10), (3, 10, 10), (4, 10, 10), (5, 10, 10), (6, 10, 10), (7, 10, 10),
                         (8, 10, 10), (9, 10, 10), (10, 10, 10);
insert into upser values (5, 100, 100), (6, 100, 100), (7, 100, 100), (8, 100, 100), (9, 100, 100), (10, 100, 100),
                         (11, 100, 100), (12, 100, 100), (13, 100, 100), (14, 100, 100), (15, 100, 100)
    on duplicate key update c2 = 2000, c3 = 2000;
select * from upser order by c1;

-- 2
truncate upser;
insert into upser values (1, 10, 10), (2, 10, 10), (3, 10, 10), (4, 10, 10), (5, 10, 10), (6, 10, 10), (7, 10, 10),
                         (8, 10, 10), (9, 10, 10), (10, 10, 10);
insert into upser values (5, 100, 100), (6, 100, 100), (7, 100, 100), (8, 100, 100), (9, 100, 100), (10, 100, 100),
                         (11, 100, 100), (12, 100, 100), (13, 100, 100), (14, 100, 100), (15, 100, 100)
                         on duplicate key update c2 = c1 + c2, c3 = c2 + c3;
select * from upser order by c1;

-- 3
truncate upser;
insert into upser values (1, 10, 10), (2, 10, 10), (3, 10, 10), (4, 10, 10), (5, 10, 10), (6, 10, 10),
                         (7, 10, 10), (8, 10, 10), (9, 10, 10), (10, 10, 10);

insert into upser values (5, c1 + 100, 100), (6, c1 + 100, 100), (7, c1 + 100, 100), (8, c1 + 100, 100),
                         (9, c1 + 100, 100), (10, c1 + 100, 100), (11, c1 + 100, 100), (12, c1 + 100, 100),
                         (13, c1 + 100, 100), (14, c1 + 100, 100), (15, c1 + 100, c1 + c2)
                         on duplicate key update c2 = c1 + c2, c3 = c2 + c3;

select * from upser order by c1;

drop table upser;

-- test var
create table with_var(a int default 999);
create function with_var_func() return int as
declare 
    a int := 666;
begin
    insert into with_var values(a);
    return a;
end;
/

call with_var_func();
select * from with_var;

drop function with_var_func;
drop table with_var;

-- test num type
create table num_default_t (
    n serial,
    c1 int default 1,
    c2 int,
    c3 tinyint default 3,
    c4 tinyint,
    c5 smallint default 5,
    c6 smallint,
    c7 integer default 7,
    c8 integer,
    c9 binary_integer default 9,
    c10 bigint default 10,
    c11 bigint,
    c12 boolean default true,
    c13 boolean,
    c14 numeric default 14.,
    c15 numeric(10, 3) default 15.,
    c16 decimal default 16,
    c17 decimal(10, 2) default 17,
    c18 double precision default 18,
    c19 float8,
    c20 float default 100 / 10,
    c21 float default 20 * (100 + 2) - 3,
    c22 float default random(),
    c23 float default random() * 100,
    c24 float
);

insert into num_default_t values(1);
insert into num_default_t values(2, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,
                                 c11, c12, c13, c14, c15, c16, c17, c18, c19, c20,
                                 c21, c22, c23, c24);
insert into num_default_t values(3, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,
                                 c11, c12, c13, c14, c15, c16, c17, c18, c19, c20,
                                 c21, c22, c23, c20);
insert into num_default_t(n, c23, c24) values(4, default, c23);

select 3, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10,
       c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, c21 
from num_default_t;

select (c23 = c24) as equal from num_default_t where n = 4;
select (c22 is null) as c22_is_null, (c23 is null) as c23_is_null from num_default_t where n = 2 or n = 3;
select (c22 is not null) as c22_is_not_null, (c23 is not null) as c23_is_not_null from num_default_t where n = 1;


-- test char type
create table char_default_t(
    n serial,
    c1 char(10) default 'char20',
    c2 char(10),
    c3 varchar(10) default 'vc3',
    c4 varchar(20),
    c5 varchar2(10) default 'vc210',
    c6 varchar2(20),
    c7 nchar(5) default 'c31',
    c8 nchar(5),
    c9 nvarchar2(5) default 'c33',
    c10 nvarchar(5) default 'c34',
    c11 varchar(20) default concat('hello', ' world'),
    c12 varchar(20)
);

insert into char_default_t values(1);
insert into char_default_t values(2, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12);
insert into char_default_t values(3, c1, c2, c3, concat(c3, ' vc4'), c5, c6, c7, c8, c9, c10, default, c11);

select * from char_default_t;

-- test time type
create table time_default_t(
   n serial,
   c1 timestamp  default '2022-12-12 22:22:22',
   c2 timestamp,
   c3 date default '2022-12-12',
   c4 date,
   c5 time default '22:22:22',
   c6 date default current_date,
   c7 date,
   c8 timestamp default current_timestamp,
   c9 timestamp,
   c10 time default current_time,
   c11 time,
   c12 time with time zone default current_time,
   c13 time
);

insert into time_default_t values(1);
insert into time_default_t values(2, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13);
insert into time_default_t values(3, default, c1,  default, c3, default, default, c6,
                                  default, c8, default, c10, default, c12);

select n, c1, c2, c3, c4, c5 from time_default_t;

select (c6 is not null) as c6_is_not_null,
       (c8 is not null) as c8_is_not_null,
       (c10 is not null) as c10_is_not_null,
       (c12 is not null) as c12_is_not_null
from time_default_t where n = 1 or n = 3;

select (c6 is not null) c6_is_not_null,
       (c8 is null) as c8_is_null,
       (c10 is null) as c10_is_null,
       (c12 is null) as c12_is_null
from time_default_t where n = 2;

select (c1=c2) as c1c2,
       (c3=c4) as c3c4,
       (c6=c7) as c6c7,
       (c8=c9) as c8c9,
       (c10=c11) as c10c11,
       (c12=c13) as c12c13
from time_default_t where n = 3;

\c postgres

drop database rightref;
