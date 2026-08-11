#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {

    TOKEN_INT_LIT,
    TOKEN_STR_LIT,
    TOKEN_CHAR_LIT,

    TOKEN_IDENT,
    TOKEN_INT,
    TOKEN_CHAR,
    TOKEN_RETURN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,

    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_ASSIGN,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_BANG,
    TOKEN_AMP,
    TOKEN_PIPE,

    TOKEN_EQ,
    TOKEN_NEQ,
    TOKEN_LEQ,
    TOKEN_GEQ,
    TOKEN_AND,
    TOKEN_OR,

    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_EOF,
    TOKEN_UNKNOWN
} TokenType;

#define MAX_LEXEME 256

typedef struct {
    TokenType   type;
    char        lexeme[MAX_LEXEME];
    int         line;
} Token;

typedef struct {
    const char* src;
    size_t      pos;
    int         line;
} Lexer;

void lexer_init(Lexer* l, const char* src);

Token next_token(Lexer* l);

const char* token_type_name(TokenType t);

#ifdef __cplusplus
}
#endif

#endif
