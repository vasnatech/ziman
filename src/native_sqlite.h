#ifndef ZIMAN_NATIVE_SQLITE_H
#define ZIMAN_NATIVE_SQLITE_H

/* Registers the `sqlite` core module's native functions (the SQLite driver:
 * open_raw/close_raw/execute_raw/query_raw) with native.c's table.
 * Called once from main() before interpret() runs. See lib/db/sqlite.zm for
 * the Ziman-side API built on these -- the natives themselves only ever
 * exchange primitives (ints, strings, arrays of tagged strings), so a
 * second driver (Postgres, over the future `net` module) can sit behind
 * the same `db` API without this file changing. */
void native_sqlite_register(void);

#endif
