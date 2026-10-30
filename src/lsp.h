#ifndef ZIMAN_LSP_H
#define ZIMAN_LSP_H

/* `ziman --lsp`: a Language Server Protocol server on stdin/stdout. Diagnostics
 * (every error the compiler front end finds, from the buffers the editor has open),
 * formatting, document symbols, go to definition, hover and completion. Returns the
 * process exit code. See docs/design/journal/, "The language server". */
int lsp_main(void);

#endif
