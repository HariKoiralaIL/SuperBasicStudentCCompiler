#ifndef CODEGEN_H
#define CODEGEN_H

#include "Parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CodeGen CodeGen;

CodeGen* codegen_create(const char* module_name);

int codegen_emit(CodeGen* cg, ASTNode* program);

int codegen_verify(CodeGen* cg);

int codegen_write_ir(CodeGen* cg, const char* path);

int codegen_write_object(CodeGen* cg, const char* path);

void codegen_free(CodeGen* cg);

#ifdef __cplusplus
}
#endif

#endif

