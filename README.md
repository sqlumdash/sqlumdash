## SQLumDash

SQLumDash is based on SQLite. Concurrency is improved by introducing the row lock feature. 

The data lock specification is changed. SQLite's transaction isolation level is Serializable. That of SQLumDash is Read Committed. When user modifies record in a table, a record lock is acquired. If it cannot get a lock, the query fails.

You can use SQLumDash as same as SQLite. API is same. Currently, it is supported only Windows (build by MSVC). Linux will be supported soon.

Row lock information is shared with processes. If a process finished unexpectedly, unnecessary lock information might be stayed. In order to unlock them, please use sqlumdash_cleaner.exe which clears all record information. If there is a process which is in a transaction, sqlumdash_cleaner.exe should be called after end the transaction.


## How to build SQLumDash

I. Generate SQLumDash source code.

1. Apply patch
```sh
  $ patch -p1 -d sqlite < patch/sqlumdash.patch
```

2. Copy new files
```sh
  $ cp src/* sqlite/src/
  $ cp -R tool/* sqlite/tool/
  $ cp test/* sqlite/test/
```

II. Build **psmalloc** module

1. Goto `sqlite` directory

2. Checkout the psmalloc repository
```sh
  $ git checkout https://github.com/sqlumdash/psmalloc.git
```
3. Goto `psmalloc` directory

4. Build psmalloc library (see [README file](https://github.com/sqlumdash/psmalloc#how-to-build-psmalloc))

5. Copy psmalloc library to psmalloc directory (see [README file](https://github.com/sqlumdash/psmalloc#how-to-build-psmalloc))

III. Build SQLumDash binary

1. Execute VisualStudio Command Prompt

2. Goto `sqlite` directory

3. Use Makefile
```sh
  $ nmake /F Makefile.msc
```

## Notices
### Table locking
DELETE-ALL requires to get a table lock. If someone are modfying the table(INSERT, DELETE and UPDATE), the other user cannot execute DELETE-ALL, and vice versa.

### Force-commit
If DDL is executed, a transaction is committeded forcibly automatically.
As same as DDL, the following command is also force-commit.
- PRAGMA incremental_vacuum

## Lisence
This software excluding sqlite is released under the MIT License, see LICENSE file.


Copyright (c) 2018 Toshiba Corporation
