#define _POSIX_C_SOURCE 200809L
#include "native_os.h"

#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "value.h"

/* os.getenv_raw(name) -> Result<string, string>: Ok(value) if the variable
 * is set (an empty value included -- set-but-empty is not the same as
 * unset), Err("") if it isn't. Ziman wraps this into an Option<string>;
 * a native can't build an Option (a user-declared enum) itself, since it
 * would need the loader's qualified name for it. */
static Value native_os_getenv_raw(Value *args, int argc, int line, int col) {
    (void)argc; (void)line; (void)col;
    Type str = type_scalar(TY_STRING);
    Type result_type = type_result(str, str);
    const char *v = getenv(value_cstr(args[0]));
    if (v == NULL) return value_result(result_type, result_obj_new(0, value_string(strdup(""))));
    return value_result(result_type, result_obj_new(1, value_string(strdup(v))));
}

void native_os_register(void) {
    native_register("os::getenv_raw", native_os_getenv_raw);
}
