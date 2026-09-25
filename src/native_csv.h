#ifndef ZIMAN_NATIVE_CSV_H
#define ZIMAN_NATIVE_CSV_H

/* Registers the `csv` core module's scanner native. Called once from main() before
 * interpret() runs. See lib/core/csv.zm. */
void native_csv_register(void);

#endif
