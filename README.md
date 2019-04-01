## SQLumDash

SQLumDash is based on SQLite. Concurrency is improved by introducing the row lock feature. 

The data lock specification is changed. SQLite's transaction isolation level is Serializable. That of SQLumDash is Read Committed. When user modifies record in a table, a record lock is acquired. If it cannot get a lock, the query fails. If DDL is executed, a transaction is committeded forcibly automatically.

You can use SQLumDash as same as SQLite. API is same. Currently, it is supported only Windows (build by MSVC). Linux will be supported soon.

Row lock information is shared with processes. If a process finished unexpectedly, unnecessary lock information might be stayed. In order to unlock them, please use sqlumdash_cleaner.exe which clears all record information. If there is a process which is in a transaction, sqlumdash_cleaner.exe should be called after end the transaction.


## How to build SQLumDash

1. Generate SQLumDash source code.

1-1. Apply patch
  patch -p1 -d sqlite < patch/sqlumdash.patch

1-2. Copy new files
  cp src/* sqlite/src/
  cp -R tool/* sqlite/tool/
  cp test/* sqlite/test/

2. Build SQLumDash binary

2-1. Execute VisualStudio Command Prompt

2-2. Goto sqlite directory

2-3. Use makefile
  nmake /F Makefile.msc

## Notices
DELETE-ALL requires to get a table lock. If someone are modfying the table(INSERT, DELETE and UPDATE), the other user cannot execute DELETE-ALL, and vice versa.

## Lisence
This software excluding sqlite is released under the MIT License, see LICENSE file.


Copyright (c) 2018 Toshiba Corporation
