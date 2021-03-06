--
-- CHAR
--

-- fixed-length by value
-- internally passed by value if <= 4 bytes in storage

SELECT char 'c' = char 'c' AS true;

--
-- Build a TABLE for testing
--

CREATE FOREIGN TABLE CHAR_TBL(f1 char) SERVER mot_server ;

INSERT INTO CHAR_TBL (f1) VALUES ('a');

INSERT INTO CHAR_TBL (f1) VALUES ('A');

-- any of the following three input formats are acceptable
INSERT INTO CHAR_TBL (f1) VALUES ('1');

INSERT INTO CHAR_TBL (f1) VALUES (2);

INSERT INTO CHAR_TBL (f1) VALUES ('3');

-- zero-length char
INSERT INTO CHAR_TBL (f1) VALUES ('');

-- try char's of greater than 1 length
INSERT INTO CHAR_TBL (f1) VALUES ('cd');
INSERT INTO CHAR_TBL (f1) VALUES ('c     ');


SELECT '' AS seven, * FROM CHAR_TBL ORDER BY f1;

SELECT '' AS six, c.*
   FROM CHAR_TBL c
   WHERE c.f1 <> 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM CHAR_TBL c
   WHERE c.f1 = 'a';

SELECT '' AS five, c.*
   FROM CHAR_TBL c
   WHERE c.f1 < 'a' ORDER BY f1;

SELECT '' AS six, c.*
   FROM CHAR_TBL c
   WHERE c.f1 <= 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM CHAR_TBL c
   WHERE c.f1 > 'a' ORDER BY f1;

SELECT '' AS two, c.*
   FROM CHAR_TBL c
   WHERE c.f1 >= 'a' ORDER BY f1;

DROP FOREIGN TABLE CHAR_TBL;

--
-- Now test longer arrays of char
--

CREATE FOREIGN TABLE CHAR_TBL(f1 char(4)) SERVER mot_server ;

INSERT INTO CHAR_TBL (f1) VALUES ('a');
INSERT INTO CHAR_TBL (f1) VALUES ('ab');
INSERT INTO CHAR_TBL (f1) VALUES ('abcd');
INSERT INTO CHAR_TBL (f1) VALUES ('abcde');
INSERT INTO CHAR_TBL (f1) VALUES ('abcd    ');

SELECT '' AS four, * FROM CHAR_TBL ORDER BY f1;

--test bpchar convert to int8
CREATE FOREIGN TABLE bpchar_int8_test(ch char(30)) SERVER mot_server ;
insert into bpchar_int8_test values('02000620238573893');
select ch from bpchar_int8_test where ch<>0 limit 10;
DROP FOREIGN TABLE bpchar_int8_test;
