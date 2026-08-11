#include "Parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ASTNode* alloc_node(NodeType kind, int line) {
    ASTNode* n = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (!n) { fprintf(stderr, "out of memory\n"); exit(1); }
    n->kind = kind;
    n->line = line;
    return n;
}

typedef struct { void** data; int len; int cap; } PtrVec;

static void vec_push(PtrVec* v, void* item) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, (size_t)v->cap * sizeof(void*));
        if (!v->data) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    v->data[v->len++] = item;
}

static void error_at(Parser* p, int line, const char* msg) {
    fprintf(stderr, "[line %d] parse error: %s (got '%s')\n",
            line, msg, p->current.lexeme);
    p->had_error = 1;
}

static Token advance(Parser* p) {
    p->previous = p->current;
    p->current  = next_token(p->lexer);
    return p->previous;
}

static int check(const Parser* p, TokenType t) {
    return p->current.type == t;
}

static int match(Parser* p, TokenType t) {
    if (!check(p, t)) return 0;
    advance(p);
    return 1;
}

static int match_any(Parser* p, const TokenType* types, int n) {
    for (int i = 0; i < n; i++) {
        if (check(p, types[i])) { advance(p); return 1; }
    }
    return 0;
}

static Token expect(Parser* p, TokenType t, const char* msg) {
    if (!check(p, t)) error_at(p, p->current.line, msg);
    return advance(p);
}

static int is_type_keyword(TokenType t) {
    return t == TOKEN_INT || t == TOKEN_CHAR;
}

static TypeInfo parse_type(Parser* p) {
    TypeInfo ti;
    ti.base      = p->current.type;
    ti.ptr_depth = 0;
    advance(p);
    while (check(p, TOKEN_STAR)) { advance(p); ti.ptr_depth++; }
    return ti;
}

static ASTNode* parse_stmt(Parser* p);
static ASTNode* parse_expr(Parser* p);
static ASTNode* parse_block(Parser* p);

static ASTNode* parse_primary(Parser* p) {
    int line = p->current.line;

    if (match(p, TOKEN_INT_LIT)) {
        ASTNode* n   = alloc_node(NODE_INT_LIT, line);
        n->int_lit.value = strtol(p->previous.lexeme, NULL, 10);
        return n;
    }

    if (match(p, TOKEN_STR_LIT)) {
        ASTNode* n = alloc_node(NODE_STR_LIT, line);
        strncpy(n->str_lit.value, p->previous.lexeme, MAX_LEXEME - 1);
        return n;
    }

    if (match(p, TOKEN_CHAR_LIT)) {
        ASTNode* n = alloc_node(NODE_CHAR_LIT, line);
        strncpy(n->str_lit.value, p->previous.lexeme, MAX_LEXEME - 1);
        return n;
    }

    if (match(p, TOKEN_IDENT)) {
        ASTNode* n = alloc_node(NODE_IDENT, line);
        strncpy(n->ident.name, p->previous.lexeme, MAX_LEXEME - 1);
        return n;
    }

    if (match(p, TOKEN_LPAREN)) {
        ASTNode* inner = parse_expr(p);
        expect(p, TOKEN_RPAREN, "expected ')' after expression");
        return inner;
    }

    error_at(p, line, "expected expression");

    ASTNode* dummy = alloc_node(NODE_INT_LIT, line);
    dummy->int_lit.value = 0;
    advance(p);
    return dummy;
}

static ASTNode* parse_postfix(Parser* p) {
    ASTNode* expr = parse_primary(p);

    while (1) {
        int line = p->current.line;

        if (match(p, TOKEN_LPAREN)) {
            ASTNode* call = alloc_node(NODE_CALL, line);
            call->call.callee    = expr;
            call->call.args      = NULL;
            call->call.arg_count = 0;

            PtrVec args = {0};
            if (!check(p, TOKEN_RPAREN)) {
                do {
                    vec_push(&args, parse_expr(p));
                } while (match(p, TOKEN_COMMA));
            }
            expect(p, TOKEN_RPAREN, "expected ')' after arguments");

            call->call.args      = (ASTNode**)args.data;
            call->call.arg_count = args.len;
            expr = call;
            continue;
        }

        if (match(p, TOKEN_LBRACKET)) {
            ASTNode* idx = alloc_node(NODE_INDEX, line);
            idx->index_expr.array = expr;
            idx->index_expr.index = parse_expr(p);
            expect(p, TOKEN_RBRACKET, "expected ']'");
            expr = idx;
            continue;
        }

        if (match(p, TOKEN_DOT)) {
            ASTNode* mem = alloc_node(NODE_MEMBER, line);
            mem->member.object = expr;
            Token field = expect(p, TOKEN_IDENT, "expected field name after '.'");
            strncpy(mem->member.field, field.lexeme, MAX_LEXEME - 1);
            expr = mem;
            continue;
        }

        break;
    }
    return expr;
}

static ASTNode* parse_unary(Parser* p) {
    static const TokenType unary_ops[] = {
        TOKEN_MINUS, TOKEN_BANG, TOKEN_STAR, TOKEN_AMP
    };
    int line = p->current.line;
    if (match_any(p, unary_ops, 4)) {
        TokenType op = p->previous.type;
        ASTNode* n   = alloc_node(NODE_UNARY, line);
        n->unary.op      = op;
        n->unary.operand = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

static ASTNode* parse_multiplicative(Parser* p) {
    ASTNode* left = parse_unary(p);
    static const TokenType ops[] = { TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT };
    while (match_any(p, ops, 3)) {
        int      line = p->previous.line;
        TokenType op  = p->previous.type;
        ASTNode* n    = alloc_node(NODE_BINARY, line);
        n->binary.op    = op;
        n->binary.left  = left;
        n->binary.right = parse_unary(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_additive(Parser* p) {
    ASTNode* left = parse_multiplicative(p);
    static const TokenType ops[] = { TOKEN_PLUS, TOKEN_MINUS };
    while (match_any(p, ops, 2)) {
        int      line = p->previous.line;
        TokenType op  = p->previous.type;
        ASTNode* n    = alloc_node(NODE_BINARY, line);
        n->binary.op    = op;
        n->binary.left  = left;
        n->binary.right = parse_multiplicative(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_relational(Parser* p) {
    ASTNode* left = parse_additive(p);
    static const TokenType ops[] = { TOKEN_LT, TOKEN_GT, TOKEN_LEQ, TOKEN_GEQ };
    while (match_any(p, ops, 4)) {
        int      line = p->previous.line;
        TokenType op  = p->previous.type;
        ASTNode* n    = alloc_node(NODE_BINARY, line);
        n->binary.op    = op;
        n->binary.left  = left;
        n->binary.right = parse_additive(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_equality(Parser* p) {
    ASTNode* left = parse_relational(p);
    static const TokenType ops[] = { TOKEN_EQ, TOKEN_NEQ };
    while (match_any(p, ops, 2)) {
        int      line = p->previous.line;
        TokenType op  = p->previous.type;
        ASTNode* n    = alloc_node(NODE_BINARY, line);
        n->binary.op    = op;
        n->binary.left  = left;
        n->binary.right = parse_relational(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_logical_and(Parser* p) {
    ASTNode* left = parse_equality(p);
    while (match(p, TOKEN_AND)) {
        int line  = p->previous.line;
        ASTNode* n = alloc_node(NODE_BINARY, line);
        n->binary.op    = TOKEN_AND;
        n->binary.left  = left;
        n->binary.right = parse_equality(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_logical_or(Parser* p) {
    ASTNode* left = parse_logical_and(p);
    while (match(p, TOKEN_OR)) {
        int line  = p->previous.line;
        ASTNode* n = alloc_node(NODE_BINARY, line);
        n->binary.op    = TOKEN_OR;
        n->binary.left  = left;
        n->binary.right = parse_logical_and(p);
        left = n;
    }
    return left;
}

static ASTNode* parse_assignment(Parser* p) {
    ASTNode* left = parse_logical_or(p);

    if (match(p, TOKEN_ASSIGN)) {
        int line  = p->previous.line;
        ASTNode* n = alloc_node(NODE_ASSIGN, line);
        n->assign.lhs = left;
        n->assign.rhs = parse_assignment(p);
        return n;
    }
    return left;
}

static ASTNode* parse_expr(Parser* p) {
    return parse_assignment(p);
}

static ASTNode* parse_block(Parser* p) {
    int line = p->current.line;
    expect(p, TOKEN_LBRACE, "expected '{'");
    ASTNode* block = alloc_node(NODE_BLOCK, line);
    PtrVec stmts = {0};
    while (!check(p, TOKEN_RBRACE) && !check(p, TOKEN_EOF)) {
        vec_push(&stmts, parse_stmt(p));
    }
    expect(p, TOKEN_RBRACE, "expected '}'");
    block->block.stmts = (ASTNode**)stmts.data;
    block->block.count = stmts.len;
    return block;
}

static ASTNode* parse_var_decl(Parser* p) {
    int line = p->current.line;
    TypeInfo ti = parse_type(p);
    Token name  = expect(p, TOKEN_IDENT, "expected variable name");

    ASTNode* n = alloc_node(NODE_VAR_DECL, line);
    n->var_decl.type = ti;
    strncpy(n->var_decl.name, name.lexeme, MAX_LEXEME - 1);
    n->var_decl.init = NULL;

    if (match(p, TOKEN_ASSIGN)) {
        n->var_decl.init = parse_expr(p);
    }
    expect(p, TOKEN_SEMICOLON, "expected ';' after variable declaration");
    return n;
}

static ASTNode* parse_return(Parser* p) {
    int line = p->current.line;
    expect(p, TOKEN_RETURN, "expected 'return'");
    ASTNode* n = alloc_node(NODE_RETURN, line);
    n->ret.expr = NULL;
    if (!check(p, TOKEN_SEMICOLON)) {
        n->ret.expr = parse_expr(p);
    }
    expect(p, TOKEN_SEMICOLON, "expected ';' after return");
    return n;
}

static ASTNode* parse_if(Parser* p) {
    int line = p->current.line;
    expect(p, TOKEN_IF, "expected 'if'");
    expect(p, TOKEN_LPAREN, "expected '(' after 'if'");
    ASTNode* cond = parse_expr(p);
    expect(p, TOKEN_RPAREN, "expected ')' after condition");

    ASTNode* n = alloc_node(NODE_IF, line);
    n->if_stmt.cond        = cond;
    n->if_stmt.then_branch = parse_stmt(p);
    n->if_stmt.else_branch = NULL;
    if (match(p, TOKEN_ELSE)) {
        n->if_stmt.else_branch = parse_stmt(p);
    }
    return n;
}

static ASTNode* parse_while(Parser* p) {
    int line = p->current.line;
    expect(p, TOKEN_WHILE, "expected 'while'");
    expect(p, TOKEN_LPAREN, "expected '(' after 'while'");
    ASTNode* cond = parse_expr(p);
    expect(p, TOKEN_RPAREN, "expected ')' after condition");

    ASTNode* n = alloc_node(NODE_WHILE, line);
    n->while_stmt.cond = cond;
    n->while_stmt.body = parse_stmt(p);
    return n;
}

static ASTNode* parse_for(Parser* p) {
    int line = p->current.line;
    expect(p, TOKEN_FOR, "expected 'for'");
    expect(p, TOKEN_LPAREN, "expected '(' after 'for'");

    ASTNode* n = alloc_node(NODE_FOR, line);
    n->for_stmt.init = NULL;
    n->for_stmt.cond = NULL;
    n->for_stmt.post = NULL;

    if (!check(p, TOKEN_SEMICOLON)) {
        if (is_type_keyword(p->current.type)) {
            n->for_stmt.init = parse_var_decl(p);
        } else {
            ASTNode* e = parse_expr(p);
            expect(p, TOKEN_SEMICOLON, "expected ';' in for-init");
            ASTNode* es = alloc_node(NODE_EXPR_STMT, e->line);
            es->expr_stmt.expr = e;
            n->for_stmt.init = es;
        }
    } else {
        advance(p);
    }

    if (!check(p, TOKEN_SEMICOLON)) {
        n->for_stmt.cond = parse_expr(p);
    }
    expect(p, TOKEN_SEMICOLON, "expected ';' after for-condition");

    if (!check(p, TOKEN_RPAREN)) {
        n->for_stmt.post = parse_expr(p);
    }
    expect(p, TOKEN_RPAREN, "expected ')' after for-clauses");

    n->for_stmt.body = parse_stmt(p);
    return n;
}

static ASTNode* parse_stmt(Parser* p) {

    if (is_type_keyword(p->current.type)) {
        return parse_var_decl(p);
    }
    if (check(p, TOKEN_RETURN)) return parse_return(p);
    if (check(p, TOKEN_IF))     return parse_if(p);
    if (check(p, TOKEN_WHILE))  return parse_while(p);
    if (check(p, TOKEN_FOR))    return parse_for(p);
    if (check(p, TOKEN_LBRACE)) return parse_block(p);

    int line = p->current.line;
    ASTNode* e  = parse_expr(p);
    expect(p, TOKEN_SEMICOLON, "expected ';' after expression statement");
    ASTNode* es = alloc_node(NODE_EXPR_STMT, line);
    es->expr_stmt.expr = e;
    return es;
}

static ASTNode* parse_func_decl(Parser* p) {
    int line = p->current.line;
    TypeInfo ret = parse_type(p);
    Token name   = expect(p, TOKEN_IDENT, "expected function name");
    expect(p, TOKEN_LPAREN, "expected '(' after function name");

    Param* params     = NULL;
    int    param_cap  = 0;
    int    param_count = 0;

    if (!check(p, TOKEN_RPAREN)) {
        do {
            if (!is_type_keyword(p->current.type)) {
                error_at(p, p->current.line, "expected type in parameter list");
                break;
            }
            if (param_count == param_cap) {
                param_cap = param_cap ? param_cap * 2 : 4;
                params = realloc(params, (size_t)param_cap * sizeof(Param));
            }
            Param* pm = &params[param_count++];
            pm->type  = parse_type(p);
            Token pname = expect(p, TOKEN_IDENT, "expected parameter name");
            strncpy(pm->name, pname.lexeme, MAX_LEXEME - 1);
            pm->name[MAX_LEXEME - 1] = '\0';
        } while (match(p, TOKEN_COMMA));
    }
    expect(p, TOKEN_RPAREN, "expected ')' after parameters");

    ASTNode* n = alloc_node(NODE_FUNC_DECL, line);
    n->func_decl.ret_type    = ret;
    strncpy(n->func_decl.name, name.lexeme, MAX_LEXEME - 1);
    n->func_decl.params      = params;
    n->func_decl.param_count = param_count;
    n->func_decl.body        = NULL;

    if (check(p, TOKEN_LBRACE)) {
        n->func_decl.body = parse_block(p);
    } else {

        expect(p, TOKEN_SEMICOLON, "expected '{' or ';' after function signature");
    }
    return n;
}

void parser_init(Parser* p, Lexer* l) {
    p->lexer     = l;
    p->had_error = 0;
    p->previous  = (Token){ TOKEN_UNKNOWN, "", 0 };
    p->current   = next_token(l);
}

ASTNode* parse(Parser* p) {
    ASTNode* root = alloc_node(NODE_PROGRAM, 1);
    PtrVec children = {0};

    while (!check(p, TOKEN_EOF)) {

        if (!is_type_keyword(p->current.type)) {
            error_at(p, p->current.line,
                     "expected type keyword at top level");
            advance(p);
            continue;
        }

        TypeInfo ti    = parse_type(p);
        Token    name  = expect(p, TOKEN_IDENT, "expected name");

        if (check(p, TOKEN_LPAREN)) {

            expect(p, TOKEN_LPAREN, "expected '('");

            Param* params      = NULL;
            int    param_cap   = 0;
            int    param_count = 0;

            if (!check(p, TOKEN_RPAREN)) {
                do {
                    if (!is_type_keyword(p->current.type)) {
                        error_at(p, p->current.line, "expected type in parameter list");
                        break;
                    }
                    if (param_count == param_cap) {
                        param_cap = param_cap ? param_cap * 2 : 4;
                        params = realloc(params, (size_t)param_cap * sizeof(Param));
                    }
                    Param* pm = &params[param_count++];
                    pm->type  = parse_type(p);
                    Token pn  = expect(p, TOKEN_IDENT, "expected parameter name");
                    strncpy(pm->name, pn.lexeme, MAX_LEXEME - 1);
                    pm->name[MAX_LEXEME - 1] = '\0';
                } while (match(p, TOKEN_COMMA));
            }
            expect(p, TOKEN_RPAREN, "expected ')'");

            ASTNode* fd = alloc_node(NODE_FUNC_DECL, name.line);
            fd->func_decl.ret_type    = ti;
            strncpy(fd->func_decl.name, name.lexeme, MAX_LEXEME - 1);
            fd->func_decl.params      = params;
            fd->func_decl.param_count = param_count;
            fd->func_decl.body        = NULL;

            if (check(p, TOKEN_LBRACE)) {
                fd->func_decl.body = parse_block(p);
            } else {
                expect(p, TOKEN_SEMICOLON, "expected '{' or ';' after function signature");
            }
            vec_push(&children, fd);
        } else {

            ASTNode* vd = alloc_node(NODE_VAR_DECL, name.line);
            vd->var_decl.type = ti;
            strncpy(vd->var_decl.name, name.lexeme, MAX_LEXEME - 1);
            vd->var_decl.init = NULL;
            if (match(p, TOKEN_ASSIGN)) {
                vd->var_decl.init = parse_expr(p);
            }
            expect(p, TOKEN_SEMICOLON, "expected ';' after global variable");
            vec_push(&children, vd);
        }
    }

    root->program.children = (ASTNode**)children.data;
    root->program.count    = children.len;
    return root;
}

void ast_free(ASTNode* node) {
    if (!node) return;
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.count; i++)
                ast_free(node->program.children[i]);
            free(node->program.children);
            break;
        case NODE_FUNC_DECL:
            free(node->func_decl.params);
            ast_free(node->func_decl.body);
            break;
        case NODE_VAR_DECL:
            ast_free(node->var_decl.init);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < node->block.count; i++)
                ast_free(node->block.stmts[i]);
            free(node->block.stmts);
            break;
        case NODE_RETURN:    ast_free(node->ret.expr);               break;
        case NODE_IF:
            ast_free(node->if_stmt.cond);
            ast_free(node->if_stmt.then_branch);
            ast_free(node->if_stmt.else_branch);
            break;
        case NODE_WHILE:
            ast_free(node->while_stmt.cond);
            ast_free(node->while_stmt.body);
            break;
        case NODE_FOR:
            ast_free(node->for_stmt.init);
            ast_free(node->for_stmt.cond);
            ast_free(node->for_stmt.post);
            ast_free(node->for_stmt.body);
            break;
        case NODE_EXPR_STMT: ast_free(node->expr_stmt.expr);          break;
        case NODE_ASSIGN:
            ast_free(node->assign.lhs);
            ast_free(node->assign.rhs);
            break;
        case NODE_BINARY:
            ast_free(node->binary.left);
            ast_free(node->binary.right);
            break;
        case NODE_UNARY:     ast_free(node->unary.operand);           break;
        case NODE_CALL:
            ast_free(node->call.callee);
            for (int i = 0; i < node->call.arg_count; i++)
                ast_free(node->call.args[i]);
            free(node->call.args);
            break;
        case NODE_INDEX:
            ast_free(node->index_expr.array);
            ast_free(node->index_expr.index);
            break;
        case NODE_MEMBER:    ast_free(node->member.object);           break;
        default: break;
    }
    free(node);
}

static void indent(int depth) {
    for (int i = 0; i < depth * 2; i++) putchar(' ');
}

const char* node_type_name(NodeType t) {
    switch (t) {
        case NODE_PROGRAM:   return "PROGRAM";
        case NODE_FUNC_DECL: return "FUNC_DECL";
        case NODE_VAR_DECL:  return "VAR_DECL";
        case NODE_BLOCK:     return "BLOCK";
        case NODE_RETURN:    return "RETURN";
        case NODE_IF:        return "IF";
        case NODE_WHILE:     return "WHILE";
        case NODE_FOR:       return "FOR";
        case NODE_EXPR_STMT: return "EXPR_STMT";
        case NODE_ASSIGN:    return "ASSIGN";
        case NODE_BINARY:    return "BINARY";
        case NODE_UNARY:     return "UNARY";
        case NODE_CALL:      return "CALL";
        case NODE_INDEX:     return "INDEX";
        case NODE_MEMBER:    return "MEMBER";
        case NODE_IDENT:     return "IDENT";
        case NODE_INT_LIT:   return "INT_LIT";
        case NODE_STR_LIT:   return "STR_LIT";
        case NODE_CHAR_LIT:  return "CHAR_LIT";
        default:             return "???";
    }
}

static const char* type_str(TypeInfo ti) {
    static char buf[64];
    const char* base = (ti.base == TOKEN_INT) ? "int" : "char";
    int off = snprintf(buf, sizeof(buf), "%s", base);
    for (int i = 0; i < ti.ptr_depth && off < 60; i++) buf[off++] = '*';
    buf[off] = '\0';
    return buf;
}

void ast_print(const ASTNode* node, int depth) {
    if (!node) return;
    indent(depth);
    switch (node->kind) {
        case NODE_PROGRAM:
            printf("PROGRAM (%d decls)\n", node->program.count);
            for (int i = 0; i < node->program.count; i++)
                ast_print(node->program.children[i], depth + 1);
            break;
        case NODE_FUNC_DECL:
            printf("FUNC_DECL %s -> %s  (%d params)\n",
                   node->func_decl.name,
                   type_str(node->func_decl.ret_type),
                   node->func_decl.param_count);
            for (int i = 0; i < node->func_decl.param_count; i++) {
                indent(depth + 1);
                printf("PARAM %s : %s\n",
                       node->func_decl.params[i].name,
                       type_str(node->func_decl.params[i].type));
            }
            ast_print(node->func_decl.body, depth + 1);
            break;
        case NODE_VAR_DECL:
            printf("VAR_DECL %s : %s\n",
                   node->var_decl.name, type_str(node->var_decl.type));
            if (node->var_decl.init) ast_print(node->var_decl.init, depth + 1);
            break;
        case NODE_BLOCK:
            printf("BLOCK (%d stmts)\n", node->block.count);
            for (int i = 0; i < node->block.count; i++)
                ast_print(node->block.stmts[i], depth + 1);
            break;
        case NODE_RETURN:
            printf("RETURN\n");
            ast_print(node->ret.expr, depth + 1);
            break;
        case NODE_IF:
            printf("IF\n");
            indent(depth + 1); printf("COND\n");
            ast_print(node->if_stmt.cond, depth + 2);
            indent(depth + 1); printf("THEN\n");
            ast_print(node->if_stmt.then_branch, depth + 2);
            if (node->if_stmt.else_branch) {
                indent(depth + 1); printf("ELSE\n");
                ast_print(node->if_stmt.else_branch, depth + 2);
            }
            break;
        case NODE_WHILE:
            printf("WHILE\n");
            indent(depth + 1); printf("COND\n");
            ast_print(node->while_stmt.cond, depth + 2);
            ast_print(node->while_stmt.body, depth + 1);
            break;
        case NODE_FOR:
            printf("FOR\n");
            indent(depth + 1); printf("INIT\n");
            ast_print(node->for_stmt.init, depth + 2);
            indent(depth + 1); printf("COND\n");
            ast_print(node->for_stmt.cond, depth + 2);
            indent(depth + 1); printf("POST\n");
            ast_print(node->for_stmt.post, depth + 2);
            indent(depth + 1); printf("BODY\n");
            ast_print(node->for_stmt.body, depth + 2);
            break;
        case NODE_EXPR_STMT:
            printf("EXPR_STMT\n");
            ast_print(node->expr_stmt.expr, depth + 1);
            break;
        case NODE_ASSIGN:
            printf("ASSIGN\n");
            ast_print(node->assign.lhs, depth + 1);
            ast_print(node->assign.rhs, depth + 1);
            break;
        case NODE_BINARY:
            printf("BINARY %s\n", token_type_name(node->binary.op));
            ast_print(node->binary.left,  depth + 1);
            ast_print(node->binary.right, depth + 1);
            break;
        case NODE_UNARY:
            printf("UNARY %s\n", token_type_name(node->unary.op));
            ast_print(node->unary.operand, depth + 1);
            break;
        case NODE_CALL:
            printf("CALL (%d args)\n", node->call.arg_count);
            ast_print(node->call.callee, depth + 1);
            for (int i = 0; i < node->call.arg_count; i++)
                ast_print(node->call.args[i], depth + 1);
            break;
        case NODE_INDEX:
            printf("INDEX\n");
            ast_print(node->index_expr.array, depth + 1);
            ast_print(node->index_expr.index, depth + 1);
            break;
        case NODE_MEMBER:
            printf("MEMBER .%s\n", node->member.field);
            ast_print(node->member.object, depth + 1);
            break;
        case NODE_IDENT:
            printf("IDENT '%s'\n", node->ident.name);
            break;
        case NODE_INT_LIT:
            printf("INT_LIT %ld\n", node->int_lit.value);
            break;
        case NODE_STR_LIT:
            printf("STR_LIT \"%s\"\n", node->str_lit.value);
            break;
        case NODE_CHAR_LIT:
            printf("CHAR_LIT '%s'\n", node->str_lit.value);
            break;
    }
}
