#include "Lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char peek(const Lexer* l) {
    return l->src[l->pos];
}

static char peek_next(const Lexer* l) {
    if (l->src[l->pos] == '\0') return '\0';
    return l->src[l->pos+1];
}

static char advance(Lexer* l) {
    char c = l->src[l->pos++];
    if (c == '\n') l->line++;
    return c;
}

static Token make_token(TokenType type, char c, int line) {
    Token t;
    t.type      = type;
    t.lexeme[0] = c;
    t.lexeme[1] = '\0';
    t.line      = line;
    return t;
}

static Token make_token_str(TokenType type, const char* start, size_t len, int line) {
    Token t;
    t.type      = type;
    t.line      = line;
    size_t copy = len < MAX_LEXEME - 1 ? len : MAX_LEXEME - 1;
    memcpy(t.lexeme, start, copy);
    t.lexeme[copy] = '\0';
    return t;
}

static void skip_whitespace_and_comments(Lexer *l) {
    while (1) {
        while (isspace((unsigned char)peek(l))) { advance(l); }

        if (peek(l) == '/' && peek_next(l) == '/') {
            while (peek(l) != '\n' && peek(l) != '\0') {
                advance(l);
            }
            continue;
        }

        if (peek(l) == '/' && peek_next(l) == '*') {
            advance(l);
            advance(l);

            while (!(peek(l) == '*' && peek_next(l) == '/') && peek(l) != '\0') {
                advance(l);
            }

            if (peek(l) != '\0') {
                advance(l);
                advance(l);
            }
            continue;
        }

        break;
    }
}

#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static const struct {
    const char* word;
    TokenType type;
} KEYWORDS[] = {
    {"int",     TOKEN_INT },
    {"char",    TOKEN_CHAR },
    {"return",  TOKEN_RETURN},
    {"if",      TOKEN_IF},
    {"else",    TOKEN_ELSE},
    {"while",   TOKEN_WHILE},
    {"for",     TOKEN_FOR},
};

static TokenType lookup_keyword(const char* lexeme) {
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (strcmp(lexeme, KEYWORDS[i].word) == 0) {
            return KEYWORDS[i].type;
        }
    }
    return TOKEN_IDENT;
}

void lexer_init(Lexer* l, const char* src) {
    l->src = src;
    l->pos = 0;
    l->line = 1;
}

Token next_token(Lexer* l) {
    skip_whitespace_and_comments(l);

    int line = l->line;
    char c   = peek(l);

    if (c == '\0') { return make_token(TOKEN_EOF, '\0', line); }

    if (isdigit((unsigned char)c)) {
        const char* start = l->src + l->pos;
        while (isdigit((unsigned char)peek(l))) { advance(l); }
        return make_token_str(TOKEN_INT_LIT, start, (size_t)(l->src + l->pos - start), line);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        const char* start = l->src + l->pos;
        while (isalnum((unsigned char)peek(l)) || peek(l) == '_') { advance(l); }
        size_t len = (size_t)(l->src + l->pos - start);
        Token t = make_token_str(TOKEN_IDENT, start, len, line);
        t.type = lookup_keyword(t.lexeme);
        return t;
    }

    if (c == '"') {
        advance(l);
        const char* start = l->src + l->pos;
        while (peek(l) != '"' && peek(l) != '\0') {
            if (peek(l) == '\\') { advance(l); }
            advance(l);
        }
        size_t len = (size_t)(l->src + l->pos - start);
        Token t = make_token_str(TOKEN_STR_LIT, start, len, line);
        if (peek(l) == '"') advance(l);
        return t;
    }

    if (c == '\'') {
        advance(l);
        const char *start = l->src + l->pos;
        if (peek(l) == '\\') advance(l);
        advance(l);
        size_t len = (size_t)(l->src + l->pos - start);
        Token t = make_token_str(TOKEN_CHAR_LIT, start, len, line);
        if (peek(l) == '\'') advance(l);
        return t;
    }

    advance(l);

    switch (c) {
        case '=': if (peek(l) == '=') { advance(l); return make_token_str(TOKEN_EQ,   "==", 2, line); }
                  return make_token(TOKEN_ASSIGN,  '=', line);
        case '!': if (peek(l) == '=') { advance(l); return make_token_str(TOKEN_NEQ,  "!=", 2, line); }
                  return make_token(TOKEN_BANG,    '!', line);
        case '<': if (peek(l) == '=') { advance(l); return make_token_str(TOKEN_LEQ,  "<=", 2, line); }
                  return make_token(TOKEN_LT,      '<', line);
        case '>': if (peek(l) == '=') { advance(l); return make_token_str(TOKEN_GEQ,  ">=", 2, line); }
                  return make_token(TOKEN_GT,      '>', line);
        case '&': if (peek(l) == '&') { advance(l); return make_token_str(TOKEN_AND,  "&&", 2, line); }
                  return make_token(TOKEN_AMP,     '&', line);
        case '|': if (peek(l) == '|') { advance(l); return make_token_str(TOKEN_OR,   "||", 2, line); }
                  return make_token(TOKEN_PIPE,    '|', line);

        case '+': return make_token(TOKEN_PLUS,     '+', line);
        case '-': return make_token(TOKEN_MINUS,    '-', line);
        case '*': return make_token(TOKEN_STAR,     '*', line);
        case '/': return make_token(TOKEN_SLASH,    '/', line);
        case '%': return make_token(TOKEN_PERCENT,  '%', line);

        case ';': return make_token(TOKEN_SEMICOLON, ';', line);
        case ',': return make_token(TOKEN_COMMA,     ',', line);
        case '.': return make_token(TOKEN_DOT,       '.', line);
        case '(': return make_token(TOKEN_LPAREN,    '(', line);
        case ')': return make_token(TOKEN_RPAREN,    ')', line);
        case '{': return make_token(TOKEN_LBRACE,    '{', line);
        case '}': return make_token(TOKEN_RBRACE,    '}', line);
        case '[': return make_token(TOKEN_LBRACKET,  '[', line);
        case ']': return make_token(TOKEN_RBRACKET,  ']', line);

        default:  return make_token(TOKEN_UNKNOWN, c, line);
    }
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOKEN_INT_LIT:   return "INT_LIT";
        case TOKEN_STR_LIT:   return "STR_LIT";
        case TOKEN_CHAR_LIT:  return "CHAR_LIT";
        case TOKEN_IDENT:     return "IDENT";
        case TOKEN_INT:       return "KW_int";
        case TOKEN_CHAR:      return "KW_char";
        case TOKEN_RETURN:    return "KW_return";
        case TOKEN_IF:        return "KW_if";
        case TOKEN_ELSE:      return "KW_else";
        case TOKEN_WHILE:     return "KW_while";
        case TOKEN_FOR:       return "KW_for";
        case TOKEN_PLUS:      return "PLUS";
        case TOKEN_MINUS:     return "MINUS";
        case TOKEN_STAR:      return "STAR";
        case TOKEN_SLASH:     return "SLASH";
        case TOKEN_PERCENT:   return "PERCENT";
        case TOKEN_ASSIGN:    return "ASSIGN";
        case TOKEN_LT:        return "LT";
        case TOKEN_GT:        return "GT";
        case TOKEN_BANG:      return "BANG";
        case TOKEN_AMP:       return "AMP";
        case TOKEN_PIPE:      return "PIPE";
        case TOKEN_EQ:        return "EQ";
        case TOKEN_NEQ:       return "NEQ";
        case TOKEN_LEQ:       return "LEQ";
        case TOKEN_GEQ:       return "GEQ";
        case TOKEN_AND:       return "AND";
        case TOKEN_OR:        return "OR";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COMMA:     return "COMMA";
        case TOKEN_DOT:       return "DOT";
        case TOKEN_LPAREN:    return "LPAREN";
        case TOKEN_RPAREN:    return "RPAREN";
        case TOKEN_LBRACE:    return "LBRACE";
        case TOKEN_RBRACE:    return "RBRACE";
        case TOKEN_LBRACKET:  return "LBRACKET";
        case TOKEN_RBRACKET:  return "RBRACKET";
        case TOKEN_EOF:       return "EOF";
        case TOKEN_UNKNOWN:   return "UNKNOWN";
        default:              return "???";
    }
}
