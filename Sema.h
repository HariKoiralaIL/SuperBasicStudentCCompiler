#ifndef SEMA_H
#define SEMA_H

#include "Parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIAG_WARNING,
    DIAG_ERROR
} DiagLevel;

typedef enum {
    SYM_VAR,
    SYM_FUNC
} SymbolKind;

typedef struct Symbol {
    char        name[MAX_LEXEME];
    SymbolKind  kind;
    TypeInfo    type;

    int         param_count;
    TypeInfo*   param_types;

    int         line;
    struct Symbol* next;
} Symbol;

#define SCOPE_BUCKETS 64

typedef struct Scope {
    Symbol*      buckets[SCOPE_BUCKETS];
    struct Scope* outer;
    int          depth;
} Scope;

typedef struct {
    Scope*  current_scope;

    Symbol* current_func;

    int     has_return;

    int     error_count;
    int     warning_count;
} Sema;

void sema_init(Sema* s);

int  sema_analyse(Sema* s, ASTNode* root);

void sema_free(Sema* s);

#ifdef __cplusplus
}
#endif

#endif
