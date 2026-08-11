#include "Sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit(Sema* s, DiagLevel level, int line, const char* msg) {
    if (level == DIAG_ERROR) {
        fprintf(stderr, "[line %d] error: %s\n", line, msg);
        s->error_count++;
    } else {
        fprintf(stderr, "[line %d] warning: %s\n", line, msg);
        s->warning_count++;
    }
}

#define ERR(s, line, ...)  do { \
    char _buf[256]; snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    emit(s, DIAG_ERROR, line, _buf); } while (0)

#define WARN(s, line, ...) do { \
    char _buf[256]; snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    emit(s, DIAG_WARNING, line, _buf); } while (0)

static TypeInfo make_type(TokenType base, int ptr_depth) {
    TypeInfo ti;
    ti.base      = base;
    ti.ptr_depth = ptr_depth;
    return ti;
}

static TypeInfo type_int(void)  { return make_type(TOKEN_INT,  0); }
static TypeInfo type_char(void) { return make_type(TOKEN_CHAR, 0); }
static TypeInfo type_unknown(void) { return make_type((TokenType)-1, 0); }

static int type_is_unknown(TypeInfo t) { return (int)t.base == -1; }

static int type_exact_eq(TypeInfo a, TypeInfo b) {
    return a.base == b.base && a.ptr_depth == b.ptr_depth;
}

static int type_is_scalar(TypeInfo t) { return t.ptr_depth == 0; }

static const char* type_name(TypeInfo t) {
    static char pool[4][64];
    static int  idx = 0;
    char* buf = pool[idx];
    idx = (idx + 1) % 4;

    const char* base = (t.base == TOKEN_INT) ? "int" : "char";
    int off = snprintf(buf, 64, "%s", base);
    for (int i = 0; i < t.ptr_depth && off < 60; i++) buf[off++] = '*';
    buf[off] = '\0';
    return buf;
}

static unsigned int hash_name(const char* name) {
    unsigned int h = 5381;
    while (*name) h = h * 33 ^ (unsigned char)*name++;
    return h % SCOPE_BUCKETS;
}

static Scope* scope_new(Scope* outer) {
    Scope* sc = (Scope*)calloc(1, sizeof(Scope));
    if (!sc) { fprintf(stderr, "out of memory\n"); exit(1); }
    sc->outer = outer;
    sc->depth = outer ? outer->depth + 1 : 0;
    return sc;
}

static Symbol* scope_insert(Scope* sc, const char* name, SymbolKind kind,
                             TypeInfo type, int line) {
    unsigned int h = hash_name(name);
    for (Symbol* sym = sc->buckets[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return NULL;
    }
    Symbol* sym = (Symbol*)calloc(1, sizeof(Symbol));
    if (!sym) { fprintf(stderr, "out of memory\n"); exit(1); }
    strncpy(sym->name, name, MAX_LEXEME - 1);
    sym->kind        = kind;
    sym->type        = type;
    sym->param_count = -1;
    sym->param_types = NULL;
    sym->line        = line;
    sym->next        = sc->buckets[h];
    sc->buckets[h]   = sym;
    return sym;
}

static Symbol* scope_lookup(Scope* sc, const char* name, int* found_depth) {
    for (Scope* cur = sc; cur; cur = cur->outer) {
        unsigned int h = hash_name(name);
        for (Symbol* sym = cur->buckets[h]; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) {
                if (found_depth) *found_depth = cur->depth;
                return sym;
            }
        }
    }
    return NULL;
}

static Symbol* scope_lookup_outer(Scope* sc, const char* name) {
    if (!sc->outer) return NULL;
    int dummy;
    return scope_lookup(sc->outer, name, &dummy);
}

static void scope_free(Scope* sc) {
    for (int i = 0; i < SCOPE_BUCKETS; i++) {
        Symbol* sym = sc->buckets[i];
        while (sym) {
            Symbol* next = sym->next;
            free(sym->param_types);
            free(sym);
            sym = next;
        }
    }
    free(sc);
}

static void push_scope(Sema* s) {
    s->current_scope = scope_new(s->current_scope);
}

static void pop_scope(Sema* s) {
    Scope* dead = s->current_scope;
    s->current_scope = dead->outer;
    scope_free(dead);
}

static TypeInfo analyse_expr_impl(Sema* s, ASTNode* node);
static TypeInfo analyse_expr(Sema* s, ASTNode* node);
static void     analyse_stmt(Sema* s, ASTNode* node);
static void     analyse_block(Sema* s, ASTNode* node);

static TypeInfo analyse_expr_impl(Sema* s, ASTNode* node) {
    switch (node->kind) {

        case NODE_INT_LIT:
            return type_int();

        case NODE_STR_LIT:

            return make_type(TOKEN_CHAR, 1);

        case NODE_CHAR_LIT:
            return type_char();

        case NODE_IDENT: {
            int depth;
            Symbol* sym = scope_lookup(s->current_scope,
                                       node->ident.name, &depth);
            if (!sym) {
                ERR(s, node->line, "undeclared identifier '%s'",
                    node->ident.name);
                return type_unknown();
            }
            return sym->type;
        }

        case NODE_ASSIGN: {
            TypeInfo lhs = analyse_expr(s, node->assign.lhs);
            TypeInfo rhs = analyse_expr(s, node->assign.rhs);
            if (type_is_unknown(lhs) || type_is_unknown(rhs))
                return type_unknown();

            if (lhs.ptr_depth != rhs.ptr_depth) {
                ERR(s, node->line,
                    "assignment type mismatch: cannot assign '%s' to '%s'",
                    type_name(rhs), type_name(lhs));
            } else if (lhs.base != rhs.base && type_is_scalar(lhs)) {
                WARN(s, node->line,
                     "implicit conversion from '%s' to '%s'",
                     type_name(rhs), type_name(lhs));
            }
            return lhs;
        }

        case NODE_BINARY: {
            TypeInfo left  = analyse_expr(s, node->binary.left);
            TypeInfo right = analyse_expr(s, node->binary.right);
            if (type_is_unknown(left) || type_is_unknown(right))
                return type_unknown();

            TokenType op = node->binary.op;

            int is_cmp = (op == TOKEN_EQ  || op == TOKEN_NEQ ||
                          op == TOKEN_LT  || op == TOKEN_GT  ||
                          op == TOKEN_LEQ || op == TOKEN_GEQ ||
                          op == TOKEN_AND || op == TOKEN_OR);
            if (is_cmp) {

                if (!type_is_scalar(left) && !type_exact_eq(left, right)) {
                    ERR(s, node->line,
                        "comparison of incompatible pointer types '%s' and '%s'",
                        type_name(left), type_name(right));
                }
                return type_int();
            }

            if (left.ptr_depth > 0 && right.ptr_depth > 0) {

                if (op != TOKEN_MINUS) {
                    ERR(s, node->line,
                        "invalid operands: '%s' and '%s' (pointer arithmetic)",
                        type_name(left), type_name(right));
                }
                return type_int();
            }
            if (left.ptr_depth > 0) {

                if (!type_is_scalar(right)) {
                    ERR(s, node->line,
                        "pointer arithmetic requires integer, got '%s'",
                        type_name(right));
                }
                return left;
            }
            if (right.ptr_depth > 0) {
                if (op != TOKEN_PLUS) {
                    ERR(s, node->line,
                        "invalid: int %s pointer", "OP");
                }
                return right;
            }

            if (left.base != right.base) {
                WARN(s, node->line,
                     "implicit conversion in binary expression: '%s' and '%s'",
                     type_name(left), type_name(right));
            }

            return (left.base == TOKEN_INT || right.base == TOKEN_INT)
                   ? type_int() : type_char();
        }

        case NODE_UNARY: {
            TypeInfo operand = analyse_expr(s, node->unary.operand);
            if (type_is_unknown(operand)) return type_unknown();

            switch (node->unary.op) {
                case TOKEN_MINUS:
                case TOKEN_BANG:
                    if (!type_is_scalar(operand)) {
                        ERR(s, node->line,
                            "unary operator requires scalar type, got '%s'",
                            type_name(operand));
                        return type_unknown();
                    }
                    return type_int();

                case TOKEN_STAR:
                    if (operand.ptr_depth == 0) {
                        ERR(s, node->line,
                            "dereference of non-pointer type '%s'",
                            type_name(operand));
                        return type_unknown();
                    }
                    return make_type(operand.base, operand.ptr_depth - 1);

                case TOKEN_AMP:
                    return make_type(operand.base, operand.ptr_depth + 1);

                default:
                    return operand;
            }
        }

        case NODE_CALL: {

            if (node->call.callee->kind != NODE_IDENT) {
                ERR(s, node->line, "callee must be a function name");
                return type_unknown();
            }
            const char* fname = node->call.callee->ident.name;
            int depth;
            Symbol* sym = scope_lookup(s->current_scope, fname, &depth);
            if (!sym) {
                ERR(s, node->line, "call to undeclared function '%s'", fname);
                return type_unknown();
            }
            if (sym->kind != SYM_FUNC) {
                ERR(s, node->line, "'%s' is not a function", fname);
                return type_unknown();
            }

            if (sym->param_count >= 0 &&
                node->call.arg_count != sym->param_count) {
                ERR(s, node->line,
                    "function '%s' expects %d argument(s), got %d",
                    fname, sym->param_count, node->call.arg_count);
            }

            int check_count = node->call.arg_count < sym->param_count
                              ? node->call.arg_count : sym->param_count;
            for (int i = 0; i < check_count; i++) {
                TypeInfo arg = analyse_expr(s, node->call.args[i]);
                TypeInfo par = sym->param_types[i];
                if (type_is_unknown(arg)) continue;
                if (par.ptr_depth != arg.ptr_depth) {
                    ERR(s, node->call.args[i]->line,
                        "argument %d to '%s': expected '%s', got '%s'",
                        i + 1, fname, type_name(par), type_name(arg));
                } else if (par.base != arg.base && type_is_scalar(par)) {
                    WARN(s, node->call.args[i]->line,
                         "argument %d to '%s': implicit conversion '%s' -> '%s'",
                         i + 1, fname, type_name(arg), type_name(par));
                }
            }

            for (int i = check_count; i < node->call.arg_count; i++) {
                analyse_expr(s, node->call.args[i]);
            }
            return sym->type;
        }

        case NODE_INDEX: {
            TypeInfo arr = analyse_expr(s, node->index_expr.array);
            TypeInfo idx = analyse_expr(s, node->index_expr.index);
            if (type_is_unknown(arr)) return type_unknown();
            if (arr.ptr_depth == 0) {
                ERR(s, node->line,
                    "subscript of non-pointer/array type '%s'",
                    type_name(arr));
                return type_unknown();
            }
            if (!type_is_unknown(idx) && !type_is_scalar(idx)) {
                ERR(s, node->line, "array index must be an integer type");
            }
            return make_type(arr.base, arr.ptr_depth - 1);
        }

        case NODE_MEMBER: {

            analyse_expr(s, node->member.object);
            ERR(s, node->line,
                "member access '.%s' unsupported (no struct types)",
                node->member.field);
            return type_unknown();
        }

        default:
            return type_unknown();
    }
}

static TypeInfo analyse_expr(Sema* s, ASTNode* node) {
    if (!node) return type_unknown();
    TypeInfo t = analyse_expr_impl(s, node);
    node->resolved_type = t;
    return t;
}

static void analyse_block(Sema* s, ASTNode* node) {
    if (!node) return;

    push_scope(s);
    for (int i = 0; i < node->block.count; i++) {
        analyse_stmt(s, node->block.stmts[i]);
    }
    pop_scope(s);
}

static void analyse_stmt(Sema* s, ASTNode* node) {
    if (!node) return;

    switch (node->kind) {

        case NODE_VAR_DECL: {

            TypeInfo init_type = type_unknown();
            if (node->var_decl.init) {
                init_type = analyse_expr(s, node->var_decl.init);
            }

            if (scope_lookup_outer(s->current_scope, node->var_decl.name)) {
                WARN(s, node->line,
                     "declaration of '%s' shadows outer variable",
                     node->var_decl.name);
            }

            Symbol* sym = scope_insert(s->current_scope,
                                       node->var_decl.name,
                                       SYM_VAR,
                                       node->var_decl.type,
                                       node->line);
            if (!sym) {
                ERR(s, node->line,
                    "redeclaration of '%s' in the same scope",
                    node->var_decl.name);
                break;
            }

            if (node->var_decl.init && !type_is_unknown(init_type)) {
                if (sym->type.ptr_depth != init_type.ptr_depth) {
                    ERR(s, node->line,
                        "initialiser type '%s' incompatible with '%s'",
                        type_name(init_type), type_name(sym->type));
                } else if (sym->type.base != init_type.base &&
                           type_is_scalar(sym->type)) {
                    WARN(s, node->line,
                         "implicit conversion from '%s' to '%s' in initialiser",
                         type_name(init_type), type_name(sym->type));
                }
            }
            break;
        }

        case NODE_BLOCK:
            analyse_block(s, node);
            break;

        case NODE_RETURN: {
            s->has_return = 1;
            if (!s->current_func) {
                ERR(s, node->line, "return outside of function");
                break;
            }
            TypeInfo expected = s->current_func->type;
            if (node->ret.expr) {
                TypeInfo actual = analyse_expr(s, node->ret.expr);
                if (!type_is_unknown(actual)) {
                    if (expected.ptr_depth != actual.ptr_depth) {
                        ERR(s, node->line,
                            "return type mismatch: expected '%s', got '%s'",
                            type_name(expected), type_name(actual));
                    } else if (expected.base != actual.base &&
                               type_is_scalar(expected)) {
                        WARN(s, node->line,
                             "implicit conversion in return: '%s' -> '%s'",
                             type_name(actual), type_name(expected));
                    }
                }
            } else {

                WARN(s, node->line,
                     "bare return in function '%s' declared to return '%s'",
                     s->current_func->name, type_name(expected));
            }
            break;
        }

        case NODE_IF: {
            TypeInfo cond = analyse_expr(s, node->if_stmt.cond);
            if (!type_is_unknown(cond) && !type_is_scalar(cond)) {
                ERR(s, node->line,
                    "if condition must be scalar, got '%s'", type_name(cond));
            }
            analyse_stmt(s, node->if_stmt.then_branch);
            if (node->if_stmt.else_branch) {
                analyse_stmt(s, node->if_stmt.else_branch);
            }
            break;
        }

        case NODE_WHILE: {
            TypeInfo cond = analyse_expr(s, node->while_stmt.cond);
            if (!type_is_unknown(cond) && !type_is_scalar(cond)) {
                ERR(s, node->line,
                    "while condition must be scalar, got '%s'", type_name(cond));
            }
            analyse_stmt(s, node->while_stmt.body);
            break;
        }

        case NODE_FOR: {

            push_scope(s);
            analyse_stmt(s, node->for_stmt.init);
            if (node->for_stmt.cond) {
                TypeInfo cond = analyse_expr(s, node->for_stmt.cond);
                if (!type_is_unknown(cond) && !type_is_scalar(cond)) {
                    ERR(s, node->line,
                        "for condition must be scalar, got '%s'",
                        type_name(cond));
                }
            }
            if (node->for_stmt.post) {
                analyse_expr(s, node->for_stmt.post);
            }

            analyse_stmt(s, node->for_stmt.body);
            pop_scope(s);
            break;
        }

        case NODE_EXPR_STMT:
            analyse_expr(s, node->expr_stmt.expr);
            break;

        default:
            break;
    }
}

static void analyse_func_decl(Sema* s, ASTNode* node) {
    const char* fname = node->func_decl.name;
    int         line  = node->line;

    int depth;
    Symbol* existing = scope_lookup(s->current_scope, fname, &depth);

    if (existing) {
        if (existing->kind != SYM_FUNC) {
            ERR(s, line, "'%s' redeclared as a function", fname);
            return;
        }

        if (!type_exact_eq(existing->type, node->func_decl.ret_type)) {
            ERR(s, line,
                "conflicting return types for '%s': previously '%s', now '%s'",
                fname,
                type_name(existing->type),
                type_name(node->func_decl.ret_type));
        }
        if (existing->param_count != node->func_decl.param_count) {
            ERR(s, line,
                "conflicting parameter count for '%s': "
                "previously %d, now %d",
                fname,
                existing->param_count,
                node->func_decl.param_count);
        }
    } else {

        Symbol* sym = scope_insert(s->current_scope, fname,
                                   SYM_FUNC,
                                   node->func_decl.ret_type, line);
        if (!sym) {
            ERR(s, line, "redeclaration of '%s'", fname);
            return;
        }
        sym->param_count = node->func_decl.param_count;
        if (sym->param_count > 0) {
            sym->param_types = (TypeInfo*)malloc(
                (size_t)sym->param_count * sizeof(TypeInfo));
            if (!sym->param_types) { fprintf(stderr, "oom\n"); exit(1); }
            for (int i = 0; i < sym->param_count; i++) {
                sym->param_types[i] = node->func_decl.params[i].type;
            }
        }
        existing = sym;
    }

    if (!node->func_decl.body) return;

    s->current_func = existing;
    s->has_return   = 0;

    push_scope(s);

    for (int i = 0; i < node->func_decl.param_count; i++) {
        Param* pm  = &node->func_decl.params[i];
        Symbol* ps = scope_insert(s->current_scope, pm->name,
                                  SYM_VAR, pm->type, line);
        if (!ps) {
            ERR(s, line, "duplicate parameter name '%s'", pm->name);
        }
    }

    ASTNode* body = node->func_decl.body;
    for (int i = 0; i < body->block.count; i++) {
        analyse_stmt(s, body->block.stmts[i]);
    }

    pop_scope(s);

    if (!s->has_return) {
        WARN(s, line,
             "function '%s' has no return statement", fname);
    }

    s->current_func = NULL;
    s->has_return   = 0;
}

void sema_init(Sema* s) {
    s->current_scope  = scope_new(NULL);
    s->current_func   = NULL;
    s->has_return     = 0;
    s->error_count    = 0;
    s->warning_count  = 0;
}

int sema_analyse(Sema* s, ASTNode* root) {
    if (!root || root->kind != NODE_PROGRAM) return -1;

    for (int i = 0; i < root->program.count; i++) {
        ASTNode* decl = root->program.children[i];
        switch (decl->kind) {
            case NODE_FUNC_DECL:
                analyse_func_decl(s, decl);
                break;
            case NODE_VAR_DECL: {

                TypeInfo init_type = type_unknown();
                if (decl->var_decl.init) {
                    init_type = analyse_expr(s, decl->var_decl.init);
                }
                Symbol* sym = scope_insert(s->current_scope,
                                           decl->var_decl.name,
                                           SYM_VAR,
                                           decl->var_decl.type,
                                           decl->line);
                if (!sym) {
                    ERR(s, decl->line,
                        "redeclaration of global '%s'", decl->var_decl.name);
                    break;
                }
                if (decl->var_decl.init && !type_is_unknown(init_type)) {
                    if (sym->type.ptr_depth != init_type.ptr_depth) {
                        ERR(s, decl->line,
                            "initialiser type '%s' incompatible with '%s'",
                            type_name(init_type), type_name(sym->type));
                    } else if (sym->type.base != init_type.base &&
                               type_is_scalar(sym->type)) {
                        WARN(s, decl->line,
                             "implicit conversion in global initialiser: "
                             "'%s' -> '%s'",
                             type_name(init_type), type_name(sym->type));
                    }
                }
                break;
            }
            default:
                ERR(s, decl->line, "unexpected top-level node");
                break;
        }
    }

    return s->error_count == 0 ? 0 : -1;
}

void sema_free(Sema* s) {
    while (s->current_scope) {
        Scope* dead = s->current_scope;
        s->current_scope = dead->outer;
        scope_free(dead);
    }
}
