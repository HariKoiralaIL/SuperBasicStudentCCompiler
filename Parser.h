#ifndef PARSER_H
#define PARSER_H

#include "Lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {

    NODE_PROGRAM,
    NODE_FUNC_DECL,
    NODE_VAR_DECL,
    NODE_BLOCK,
    NODE_RETURN,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_EXPR_STMT,

    NODE_ASSIGN,
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,
    NODE_INDEX,
    NODE_MEMBER,
    NODE_IDENT,
    NODE_INT_LIT,
    NODE_STR_LIT,
    NODE_CHAR_LIT,
} NodeType;

typedef struct TypeInfo {
    TokenType   base;
    int         ptr_depth;
} TypeInfo;

typedef struct ASTNode ASTNode;

typedef struct Param {
    TypeInfo    type;
    char        name[MAX_LEXEME];
} Param;

struct ASTNode {
    NodeType    kind;
    int         line;

    TypeInfo    resolved_type;

    union {

        struct {
            ASTNode**   children;
            int         count;
        } program;

        struct {
            TypeInfo    ret_type;
            char        name[MAX_LEXEME];
            Param*      params;
            int         param_count;
            ASTNode*    body;
        } func_decl;

        struct {
            TypeInfo    type;
            char        name[MAX_LEXEME];
            ASTNode*    init;
        } var_decl;

        struct {
            ASTNode**   stmts;
            int         count;
        } block;

        struct {
            ASTNode*    expr;
        } ret;

        struct {
            ASTNode*    cond;
            ASTNode*    then_branch;
            ASTNode*    else_branch;
        } if_stmt;

        struct {
            ASTNode*    cond;
            ASTNode*    body;
        } while_stmt;

        struct {
            ASTNode*    init;
            ASTNode*    cond;
            ASTNode*    post;
            ASTNode*    body;
        } for_stmt;

        struct {
            ASTNode*    expr;
        } expr_stmt;

        struct {
            ASTNode*    lhs;
            ASTNode*    rhs;
        } assign;

        struct {
            TokenType   op;
            ASTNode*    left;
            ASTNode*    right;
        } binary;

        struct {
            TokenType   op;
            ASTNode*    operand;
        } unary;

        struct {
            ASTNode*    callee;
            ASTNode**   args;
            int         arg_count;
        } call;

        struct {
            ASTNode*    array;
            ASTNode*    index;
        } index_expr;

        struct {
            ASTNode*    object;
            char        field[MAX_LEXEME];
        } member;

        struct {
            char        name[MAX_LEXEME];
        } ident;

        struct {
            long        value;
        } int_lit;

        struct {
            char        value[MAX_LEXEME];
        } str_lit;
    };
};

typedef struct {
    Lexer*  lexer;
    Token   current;
    Token   previous;
    int     had_error;
} Parser;

void    parser_init(Parser* p, Lexer* l);

ASTNode* parse(Parser* p);

void    ast_free(ASTNode* node);

void    ast_print(const ASTNode* node, int depth);

const char* node_type_name(NodeType t);

#ifdef __cplusplus
}
#endif

#endif
