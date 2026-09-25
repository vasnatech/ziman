#ifndef ZIMAN_NATIVE_JSON_H
#define ZIMAN_NATIVE_JSON_H

/* Registers the `json` core module's native parser and serializer. Called once from main()
 * before interpret() runs. See lib/core/json.zm. */
void native_json_register(void);

#endif
