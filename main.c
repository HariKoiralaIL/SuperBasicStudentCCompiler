#include "CodeGen.h"
#include "Lexer.h"
#include "Parser.h"
#include "Sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <source.c> [-o out.o] [--emit-llvm out.ll]\n", argv[0]);
        return 1;
    }

    const char* src_path = argv[1];
    const char* obj_path = "out.o";
    const char* ir_path  = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) obj_path = argv[++i];
        else if (strcmp(argv[i], "--emit-llvm") == 0 && i + 1 < argc) ir_path = argv[++i];
    }

    char* src = read_file(src_path);

    Lexer lexer;
    lexer_init(&lexer, src);

    Parser parser;
    parser_init(&parser, &lexer);

    ASTNode* ast = parse(&parser);
    if (!ast || parser.had_error) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }

    Sema sema;
    sema_init(&sema);
    int sema_rc = sema_analyse(&sema, ast);
    fprintf(stderr, "sema: %d error(s), %d warning(s)\n", sema.error_count, sema.warning_count);
    if (sema_rc != 0) {
        sema_free(&sema);
        ast_free(ast);
        return 1;
    }

    CodeGen* cg = codegen_create(src_path);
    int cg_rc = codegen_emit(cg, ast);
    if (cg_rc == 0) cg_rc = codegen_verify(cg);

    if (cg_rc == 0 && ir_path) codegen_write_ir(cg, ir_path);
    if (cg_rc == 0) cg_rc = codegen_write_object(cg, obj_path);

    codegen_free(cg);
    sema_free(&sema);
    ast_free(ast);
    free(src);

    if (cg_rc != 0) { fprintf(stderr, "codegen failed\n"); return 1; }

    fprintf(stderr, "wrote %s%s%s\n", obj_path, ir_path ? " and " : "", ir_path ? ir_path : "");
    return 0;
}

