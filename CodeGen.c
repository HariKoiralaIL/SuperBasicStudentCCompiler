#include "CodeGen.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CGVar {
    char            name[MAX_LEXEME];
    LLVMValueRef    addr;
    TypeInfo        type;
    struct CGVar*   next;
} CGVar;

typedef struct CGScope {
    CGVar*          vars;
    struct CGScope* outer;
} CGScope;

typedef struct CGFunc {
    char            name[MAX_LEXEME];
    LLVMValueRef    fn;
    LLVMTypeRef     fn_type;
    TypeInfo        ret_type;
    struct CGFunc*  next;
} CGFunc;

struct CodeGen {
    LLVMContextRef  ctx;
    LLVMModuleRef   mod;
    LLVMBuilderRef  builder;

    CGScope*        scope;
    CGFunc*         funcs;

    LLVMValueRef    cur_fn;
    TypeInfo        cur_ret_type;

    int             error_count;
};

static void cg_error(CodeGen* cg, int line, const char* msg) {
    fprintf(stderr, "[line %d] codegen error: %s\n", line, msg);
    cg->error_count++;
}

static void push_scope(CodeGen* cg) {
    CGScope* sc = (CGScope*)calloc(1, sizeof(CGScope));
    sc->outer = cg->scope;
    cg->scope = sc;
}

static void pop_scope(CodeGen* cg) {
    CGScope* dead = cg->scope;
    cg->scope = dead->outer;
    CGVar* v = dead->vars;
    while (v) { CGVar* next = v->next; free(v); v = next; }
    free(dead);
}

static void scope_define(CodeGen* cg, const char* name, LLVMValueRef addr, TypeInfo type) {
    CGVar* v = (CGVar*)calloc(1, sizeof(CGVar));
    strncpy(v->name, name, MAX_LEXEME - 1);
    v->addr = addr;
    v->type = type;
    v->next = cg->scope->vars;
    cg->scope->vars = v;
}

static CGVar* scope_lookup(CodeGen* cg, const char* name) {
    for (CGScope* sc = cg->scope; sc; sc = sc->outer) {
        for (CGVar* v = sc->vars; v; v = v->next) {
            if (strcmp(v->name, name) == 0) return v;
        }
    }
    return NULL;
}

static CGFunc* func_lookup(CodeGen* cg, const char* name) {
    for (CGFunc* f = cg->funcs; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

static LLVMTypeRef llvm_type_of(CodeGen* cg, TypeInfo t) {
    LLVMTypeRef base = (t.base == TOKEN_CHAR)
        ? LLVMInt8TypeInContext(cg->ctx)
        : LLVMInt32TypeInContext(cg->ctx);
    if (t.ptr_depth > 0) return LLVMPointerTypeInContext(cg->ctx, 0);
    return base;
}

static TypeInfo pointee_of(TypeInfo t) {
    TypeInfo p = t;
    p.ptr_depth -= 1;
    return p;
}

static int block_terminated(LLVMBuilderRef b) {
    LLVMBasicBlockRef bb = LLVMGetInsertBlock(b);
    return bb && LLVMGetBasicBlockTerminator(bb) != NULL;
}

static size_t decode_escapes(const char* in, char* out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < outcap; i++) {
        if (in[i] == '\\' && in[i + 1] != '\0') {
            i++;
            switch (in[i]) {
                case 'n':  out[o++] = '\n'; break;
                case 't':  out[o++] = '\t'; break;
                case 'r':  out[o++] = '\r'; break;
                case '0':  out[o++] = '\0'; break;
                case '\\': out[o++] = '\\'; break;
                case '\'': out[o++] = '\''; break;
                case '"':  out[o++] = '"';  break;
                default:   out[o++] = in[i]; break;
            }
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return o;
}

static LLVMValueRef cg_expr(CodeGen* cg, ASTNode* node);
static LLVMValueRef cg_lvalue(CodeGen* cg, ASTNode* node);
static void          cg_stmt(CodeGen* cg, ASTNode* node);

static LLVMValueRef coerce(CodeGen* cg, LLVMValueRef v, TypeInfo from, TypeInfo to) {
    if (from.ptr_depth > 0 || to.ptr_depth > 0) return v;
    LLVMTypeRef to_ty = llvm_type_of(cg, to);
    unsigned from_bits = (from.base == TOKEN_CHAR) ? 8 : 32;
    unsigned to_bits   = (to.base   == TOKEN_CHAR) ? 8 : 32;
    if (from_bits == to_bits) return v;
    if (from_bits < to_bits)  return LLVMBuildSExt(cg->builder, v, to_ty, "sext");
    return LLVMBuildTrunc(cg->builder, v, to_ty, "trunc");
}

static LLVMValueRef cg_lvalue(CodeGen* cg, ASTNode* node) {
    switch (node->kind) {
        case NODE_IDENT: {
            CGVar* v = scope_lookup(cg, node->ident.name);
            if (!v) {
                cg_error(cg, node->line, "internal: unresolved identifier reached codegen");
                return NULL;
            }
            return v->addr;
        }
        case NODE_UNARY:
            if (node->unary.op == TOKEN_STAR) {

                return cg_expr(cg, node->unary.operand);
            }
            break;
        case NODE_INDEX: {
            LLVMValueRef base = cg_expr(cg, node->index_expr.array);
            LLVMValueRef idx  = cg_expr(cg, node->index_expr.index);
            TypeInfo elem_type = node->resolved_type;
            LLVMTypeRef elem_llvm = llvm_type_of(cg, elem_type);
            LLVMValueRef indices[1] = { idx };
            return LLVMBuildGEP2(cg->builder, elem_llvm, base, indices, 1, "idx");
        }
        default:
            break;
    }
    cg_error(cg, node->line, "internal: expression is not an lvalue");
    return NULL;
}

static LLVMValueRef cg_binary(CodeGen* cg, ASTNode* node) {
    TokenType op = node->binary.op;

    if (op == TOKEN_AND || op == TOKEN_OR) {
        LLVMValueRef lhs = cg_expr(cg, node->binary.left);
        LLVMValueRef lhs_bool = LLVMBuildICmp(cg->builder, LLVMIntNE, lhs,
            LLVMConstInt(LLVMTypeOf(lhs), 0, 0), "lhs.bool");

        LLVMBasicBlockRef rhs_bb   = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "sc.rhs");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "sc.merge");
        LLVMBasicBlockRef lhs_bb   = LLVMGetInsertBlock(cg->builder);

        if (op == TOKEN_AND) LLVMBuildCondBr(cg->builder, lhs_bool, rhs_bb, merge_bb);
        else                 LLVMBuildCondBr(cg->builder, lhs_bool, merge_bb, rhs_bb);

        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef rhs = cg_expr(cg, node->binary.right);
        LLVMValueRef rhs_bool = LLVMBuildICmp(cg->builder, LLVMIntNE, rhs,
            LLVMConstInt(LLVMTypeOf(rhs), 0, 0), "rhs.bool");
        LLVMBasicBlockRef rhs_end_bb = LLVMGetInsertBlock(cg->builder);
        if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, merge_bb);

        LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "sc.result");
        LLVMValueRef short_circuit_val = LLVMConstInt(LLVMInt1TypeInContext(cg->ctx),
            op == TOKEN_AND ? 0 : 1, 0);
        LLVMValueRef incoming_vals[2] = { short_circuit_val, rhs_bool };
        LLVMBasicBlockRef incoming_bbs[2] = { lhs_bb, rhs_end_bb };
        LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
        return LLVMBuildZExt(cg->builder, phi, LLVMInt32TypeInContext(cg->ctx), "sc.i32");
    }

    TypeInfo lt = node->binary.left->resolved_type;
    TypeInfo rt = node->binary.right->resolved_type;
    LLVMValueRef l = cg_expr(cg, node->binary.left);
    LLVMValueRef r = cg_expr(cg, node->binary.right);

    if (lt.ptr_depth > 0 && rt.ptr_depth == 0 && (op == TOKEN_PLUS || op == TOKEN_MINUS)) {
        LLVMTypeRef elem = llvm_type_of(cg, pointee_of(lt));
        LLVMValueRef idx = (op == TOKEN_MINUS) ? LLVMBuildNeg(cg->builder, r, "neg") : r;
        LLVMValueRef indices[1] = { idx };
        return LLVMBuildGEP2(cg->builder, elem, l, indices, 1, "ptradd");
    }
    if (lt.ptr_depth > 0 && rt.ptr_depth > 0 && op == TOKEN_MINUS) {

        LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
        LLVMValueRef li = LLVMBuildPtrToInt(cg->builder, l, i64, "li");
        LLVMValueRef ri = LLVMBuildPtrToInt(cg->builder, r, i64, "ri");
        LLVMValueRef diff = LLVMBuildSub(cg->builder, li, ri, "diff");
        unsigned esz = (lt.base == TOKEN_CHAR) ? 1 : 4;
        LLVMValueRef scaled = LLVMBuildSDiv(cg->builder, diff,
            LLVMConstInt(i64, esz, 0), "scaled");
        return LLVMBuildTrunc(cg->builder, scaled, LLVMInt32TypeInContext(cg->ctx), "diff32");
    }

    if (lt.ptr_depth == 0 && rt.ptr_depth == 0) {
        TypeInfo common = (lt.base == TOKEN_INT || rt.base == TOKEN_INT)
            ? lt.base == TOKEN_INT ? lt : rt
            : lt;
        l = coerce(cg, l, lt, common);
        r = coerce(cg, r, rt, common);
    }

    switch (op) {
        case TOKEN_PLUS:    return LLVMBuildAdd (cg->builder, l, r, "add");
        case TOKEN_MINUS:   return LLVMBuildSub (cg->builder, l, r, "sub");
        case TOKEN_STAR:    return LLVMBuildMul (cg->builder, l, r, "mul");
        case TOKEN_SLASH:   return LLVMBuildSDiv(cg->builder, l, r, "div");
        case TOKEN_PERCENT: return LLVMBuildSRem(cg->builder, l, r, "rem");
        case TOKEN_EQ:  case TOKEN_NEQ:
        case TOKEN_LT:  case TOKEN_GT:
        case TOKEN_LEQ: case TOKEN_GEQ: {
            LLVMIntPredicate pred =
                op == TOKEN_EQ  ? LLVMIntEQ  :
                op == TOKEN_NEQ ? LLVMIntNE  :
                op == TOKEN_LT  ? LLVMIntSLT :
                op == TOKEN_GT  ? LLVMIntSGT :
                op == TOKEN_LEQ ? LLVMIntSLE : LLVMIntSGE;
            LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, l, r, "cmp");
            return LLVMBuildZExt(cg->builder, cmp, LLVMInt32TypeInContext(cg->ctx), "cmp.i32");
        }
        default:
            cg_error(cg, node->line, "internal: unhandled binary operator");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

static LLVMValueRef cg_expr(CodeGen* cg, ASTNode* node) {
    if (!node) return NULL;

    switch (node->kind) {
        case NODE_INT_LIT:
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned long long)node->int_lit.value, 1);

        case NODE_CHAR_LIT: {
            char buf[MAX_LEXEME];
            decode_escapes(node->str_lit.value, buf, sizeof(buf));
            return LLVMConstInt(LLVMInt8TypeInContext(cg->ctx), (unsigned char)buf[0], 0);
        }

        case NODE_STR_LIT: {
            char buf[MAX_LEXEME];
            decode_escapes(node->str_lit.value, buf, sizeof(buf));
            return LLVMBuildGlobalStringPtr(cg->builder, buf, "str");
        }

        case NODE_IDENT: {
            CGVar* v = scope_lookup(cg, node->ident.name);
            if (!v) {
                cg_error(cg, node->line, "internal: unresolved identifier reached codegen");
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMTypeRef ty = llvm_type_of(cg, v->type);
            return LLVMBuildLoad2(cg->builder, ty, v->addr, node->ident.name);
        }

        case NODE_ASSIGN: {
            LLVMValueRef addr = cg_lvalue(cg, node->assign.lhs);
            LLVMValueRef val  = cg_expr(cg, node->assign.rhs);
            val = coerce(cg, val, node->assign.rhs->resolved_type, node->assign.lhs->resolved_type);
            LLVMBuildStore(cg->builder, val, addr);
            return val;
        }

        case NODE_BINARY:
            return cg_binary(cg, node);

        case NODE_UNARY: {
            switch (node->unary.op) {
                case TOKEN_AMP:
                    return cg_lvalue(cg, node->unary.operand);
                case TOKEN_STAR: {
                    LLVMValueRef ptr = cg_expr(cg, node->unary.operand);
                    LLVMTypeRef elem = llvm_type_of(cg, node->resolved_type);
                    return LLVMBuildLoad2(cg->builder, elem, ptr, "deref");
                }
                case TOKEN_MINUS: {
                    LLVMValueRef v = cg_expr(cg, node->unary.operand);
                    v = coerce(cg, v, node->unary.operand->resolved_type, node->resolved_type);
                    return LLVMBuildNeg(cg->builder, v, "neg");
                }
                case TOKEN_BANG: {
                    LLVMValueRef v = cg_expr(cg, node->unary.operand);
                    LLVMValueRef is_zero = LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                        LLVMConstInt(LLVMTypeOf(v), 0, 0), "not");
                    return LLVMBuildZExt(cg->builder, is_zero, LLVMInt32TypeInContext(cg->ctx), "not.i32");
                }
                default:
                    cg_error(cg, node->line, "internal: unhandled unary operator");
                    return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
        }

        case NODE_CALL: {
            const char* fname = node->call.callee->ident.name;
            CGFunc* f = func_lookup(cg, fname);
            if (!f) {
                cg_error(cg, node->line, "internal: unresolved call reached codegen");
                return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
            }
            LLVMValueRef* args = NULL;
            if (node->call.arg_count > 0) {
                args = (LLVMValueRef*)malloc(sizeof(LLVMValueRef) * (size_t)node->call.arg_count);
            }
            for (int i = 0; i < node->call.arg_count; i++) {
                args[i] = cg_expr(cg, node->call.args[i]);
            }
            LLVMValueRef call = LLVMBuildCall2(cg->builder, f->fn_type, f->fn,
                args, (unsigned)node->call.arg_count, "call");
            free(args);
            return call;
        }

        case NODE_INDEX: {
            LLVMValueRef addr = cg_lvalue(cg, node);
            LLVMTypeRef elem = llvm_type_of(cg, node->resolved_type);
            return LLVMBuildLoad2(cg->builder, elem, addr, "elem");
        }

        default:
            cg_error(cg, node->line, "internal: unhandled expression node in codegen");
            return LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0);
    }
}

static void cg_block(CodeGen* cg, ASTNode* node) {
    push_scope(cg);
    for (int i = 0; i < node->block.count; i++) {
        if (block_terminated(cg->builder)) break;
        cg_stmt(cg, node->block.stmts[i]);
    }
    pop_scope(cg);
}

static void cg_stmt(CodeGen* cg, ASTNode* node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_VAR_DECL: {
            LLVMTypeRef ty = llvm_type_of(cg, node->var_decl.type);
            LLVMValueRef addr = LLVMBuildAlloca(cg->builder, ty, node->var_decl.name);
            scope_define(cg, node->var_decl.name, addr, node->var_decl.type);
            if (node->var_decl.init) {
                LLVMValueRef v = cg_expr(cg, node->var_decl.init);
                v = coerce(cg, v, node->var_decl.init->resolved_type, node->var_decl.type);
                LLVMBuildStore(cg->builder, v, addr);
            }
            break;
        }

        case NODE_BLOCK:
            cg_block(cg, node);
            break;

        case NODE_RETURN: {
            if (node->ret.expr) {
                LLVMValueRef v = cg_expr(cg, node->ret.expr);
                v = coerce(cg, v, node->ret.expr->resolved_type, cg->cur_ret_type);
                LLVMBuildRet(cg->builder, v);
            } else {
                LLVMBuildRet(cg->builder, LLVMConstInt(llvm_type_of(cg, cg->cur_ret_type), 0, 0));
            }
            break;
        }

        case NODE_IF: {
            LLVMValueRef cond = cg_expr(cg, node->if_stmt.cond);
            LLVMValueRef cond_bool = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                LLVMConstInt(LLVMTypeOf(cond), 0, 0), "if.cond");

            LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "if.then");
            LLVMBasicBlockRef else_bb  = node->if_stmt.else_branch
                ? LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "if.else") : NULL;
            LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "if.end");

            LLVMBuildCondBr(cg->builder, cond_bool, then_bb, else_bb ? else_bb : merge_bb);

            LLVMPositionBuilderAtEnd(cg->builder, then_bb);
            cg_stmt(cg, node->if_stmt.then_branch);
            if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, merge_bb);

            if (else_bb) {
                LLVMPositionBuilderAtEnd(cg->builder, else_bb);
                cg_stmt(cg, node->if_stmt.else_branch);
                if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, merge_bb);
            }

            LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
            break;
        }

        case NODE_WHILE: {
            LLVMBasicBlockRef cond_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "while.cond");
            LLVMBasicBlockRef body_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "while.body");
            LLVMBasicBlockRef end_bb   = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "while.end");

            LLVMBuildBr(cg->builder, cond_bb);
            LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
            LLVMValueRef cond = cg_expr(cg, node->while_stmt.cond);
            LLVMValueRef cond_bool = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                LLVMConstInt(LLVMTypeOf(cond), 0, 0), "while.test");
            LLVMBuildCondBr(cg->builder, cond_bool, body_bb, end_bb);

            LLVMPositionBuilderAtEnd(cg->builder, body_bb);
            cg_stmt(cg, node->while_stmt.body);
            if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, cond_bb);

            LLVMPositionBuilderAtEnd(cg->builder, end_bb);
            break;
        }

        case NODE_FOR: {
            push_scope(cg);

            LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "for.cond");
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "for.body");
            LLVMBasicBlockRef post_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "for.post");
            LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn, "for.end");

            if (node->for_stmt.init) cg_stmt(cg, node->for_stmt.init);
            LLVMBuildBr(cg->builder, cond_bb);

            LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
            if (node->for_stmt.cond) {
                LLVMValueRef cond = cg_expr(cg, node->for_stmt.cond);
                LLVMValueRef cond_bool = LLVMBuildICmp(cg->builder, LLVMIntNE, cond,
                    LLVMConstInt(LLVMTypeOf(cond), 0, 0), "for.test");
                LLVMBuildCondBr(cg->builder, cond_bool, body_bb, end_bb);
            } else {
                LLVMBuildBr(cg->builder, body_bb);
            }

            LLVMPositionBuilderAtEnd(cg->builder, body_bb);
            cg_stmt(cg, node->for_stmt.body);
            if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, post_bb);

            LLVMPositionBuilderAtEnd(cg->builder, post_bb);
            if (node->for_stmt.post) cg_expr(cg, node->for_stmt.post);
            if (!block_terminated(cg->builder)) LLVMBuildBr(cg->builder, cond_bb);

            LLVMPositionBuilderAtEnd(cg->builder, end_bb);
            pop_scope(cg);
            break;
        }

        case NODE_EXPR_STMT:
            cg_expr(cg, node->expr_stmt.expr);
            break;

        default:
            cg_error(cg, node->line, "internal: unhandled statement node in codegen");
            break;
    }
}

static LLVMTypeRef build_fn_type(CodeGen* cg, ASTNode* fdecl, TypeInfo* out_ret) {
    LLVMTypeRef* params = NULL;
    if (fdecl->func_decl.param_count > 0) {
        params = (LLVMTypeRef*)malloc(sizeof(LLVMTypeRef) * (size_t)fdecl->func_decl.param_count);
        for (int i = 0; i < fdecl->func_decl.param_count; i++) {
            params[i] = llvm_type_of(cg, fdecl->func_decl.params[i].type);
        }
    }
    LLVMTypeRef ret = llvm_type_of(cg, fdecl->func_decl.ret_type);
    LLVMTypeRef fn_ty = LLVMFunctionType(ret, params, (unsigned)fdecl->func_decl.param_count, 0);
    free(params);
    if (out_ret) *out_ret = fdecl->func_decl.ret_type;
    return fn_ty;
}

static void declare_func(CodeGen* cg, ASTNode* fdecl) {
    const char* name = fdecl->func_decl.name;
    if (func_lookup(cg, name)) return;

    TypeInfo ret_type;
    LLVMTypeRef fn_ty = build_fn_type(cg, fdecl, &ret_type);
    LLVMValueRef fn = LLVMAddFunction(cg->mod, name, fn_ty);

    CGFunc* f = (CGFunc*)calloc(1, sizeof(CGFunc));
    strncpy(f->name, name, MAX_LEXEME - 1);
    f->fn = fn;
    f->fn_type = fn_ty;
    f->ret_type = ret_type;
    f->next = cg->funcs;
    cg->funcs = f;
}

static void define_func_body(CodeGen* cg, ASTNode* fdecl) {
    if (!fdecl->func_decl.body) return;

    CGFunc* f = func_lookup(cg, fdecl->func_decl.name);
    cg->cur_fn = f->fn;
    cg->cur_ret_type = f->ret_type;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, f->fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    push_scope(cg);
    for (int i = 0; i < fdecl->func_decl.param_count; i++) {
        Param* pm = &fdecl->func_decl.params[i];
        LLVMTypeRef pty = llvm_type_of(cg, pm->type);
        LLVMValueRef addr = LLVMBuildAlloca(cg->builder, pty, pm->name);
        LLVMBuildStore(cg->builder, LLVMGetParam(f->fn, (unsigned)i), addr);
        scope_define(cg, pm->name, addr, pm->type);
    }

    ASTNode* body = fdecl->func_decl.body;
    for (int i = 0; i < body->block.count; i++) {
        if (block_terminated(cg->builder)) break;
        cg_stmt(cg, body->block.stmts[i]);
    }
    pop_scope(cg);

    if (!block_terminated(cg->builder)) {
        LLVMBuildRet(cg->builder, LLVMConstInt(llvm_type_of(cg, cg->cur_ret_type), 0, 0));
    }

    cg->cur_fn = NULL;
}

static void define_global_var(CodeGen* cg, ASTNode* decl) {
    LLVMTypeRef ty = llvm_type_of(cg, decl->var_decl.type);
    LLVMValueRef g = LLVMAddGlobal(cg->mod, ty, decl->var_decl.name);

    LLVMValueRef init = NULL;
    ASTNode* iv = decl->var_decl.init;
    if (iv && iv->kind == NODE_INT_LIT) {
        init = LLVMConstInt(ty, (unsigned long long)iv->int_lit.value, 1);
    } else if (iv && iv->kind == NODE_CHAR_LIT) {
        char buf[MAX_LEXEME];
        decode_escapes(iv->str_lit.value, buf, sizeof(buf));
        init = LLVMConstInt(ty, (unsigned char)buf[0], 0);
    } else if (iv && iv->kind == NODE_STR_LIT) {

        char buf[MAX_LEXEME];
        size_t len = decode_escapes(iv->str_lit.value, buf, sizeof(buf));
        LLVMValueRef str_const = LLVMConstStringInContext(cg->ctx, buf, (unsigned)len, 0);
        LLVMValueRef backing = LLVMAddGlobal(cg->mod, LLVMTypeOf(str_const), ".str");
        LLVMSetInitializer(backing, str_const);
        LLVMSetGlobalConstant(backing, 1);
        LLVMSetLinkage(backing, LLVMPrivateLinkage);
        init = backing;
    } else {
        if (iv) {
            cg_error(cg, decl->line,
                "global initialiser must be a constant literal (this minimal codegen "
                "does not support non-constant global initialisers)");
        }
        init = LLVMConstNull(ty);
    }
    LLVMSetInitializer(g, init);

    scope_define(cg, decl->var_decl.name, g, decl->var_decl.type);
}

int codegen_emit(CodeGen* cg, ASTNode* program) {
    if (!program || program->kind != NODE_PROGRAM) {
        cg_error(cg, 0, "internal: expected a NODE_PROGRAM root");
        return -1;
    }

    push_scope(cg);

    for (int i = 0; i < program->program.count; i++) {
        ASTNode* decl = program->program.children[i];
        if (decl->kind == NODE_FUNC_DECL) declare_func(cg, decl);
    }

    for (int i = 0; i < program->program.count; i++) {
        ASTNode* decl = program->program.children[i];
        if (decl->kind == NODE_FUNC_DECL) define_func_body(cg, decl);
        else if (decl->kind == NODE_VAR_DECL) define_global_var(cg, decl);
    }

    pop_scope(cg);
    return cg->error_count == 0 ? 0 : -1;
}

CodeGen* codegen_create(const char* module_name) {
    CodeGen* cg = (CodeGen*)calloc(1, sizeof(CodeGen));
    cg->ctx     = LLVMContextCreate();
    cg->mod     = LLVMModuleCreateWithNameInContext(module_name, cg->ctx);
    cg->builder = LLVMCreateBuilderInContext(cg->ctx);
    return cg;
}

int codegen_verify(CodeGen* cg) {
    char* err = NULL;
    LLVMBool bad = LLVMVerifyModule(cg->mod, LLVMPrintMessageAction, &err);
    if (err) LLVMDisposeMessage(err);
    return bad ? -1 : 0;
}

int codegen_write_ir(CodeGen* cg, const char* path) {
    char* err = NULL;
    if (LLVMPrintModuleToFile(cg->mod, path, &err)) {
        fprintf(stderr, "failed to write IR: %s\n", err ? err : "unknown error");
        if (err) LLVMDisposeMessage(err);
        return -1;
    }
    return 0;
}

int codegen_write_object(CodeGen* cg, const char* path) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();

    char* triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target;
    char* err = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &err)) {
        fprintf(stderr, "failed to get target: %s\n", err ? err : "unknown error");
        if (err) LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        return -1;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        LLVMCodeGenLevelNone, LLVMRelocDefault, LLVMCodeModelDefault);

    LLVMSetTarget(cg->mod, triple);
    LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
    char* dl_str = LLVMCopyStringRepOfTargetData(dl);
    LLVMSetDataLayout(cg->mod, dl_str);
    LLVMDisposeMessage(dl_str);
    LLVMDisposeTargetData(dl);

    int rc = 0;
    if (LLVMTargetMachineEmitToFile(tm, cg->mod, path, LLVMObjectFile, &err)) {
        fprintf(stderr, "failed to emit object file: %s\n", err ? err : "unknown error");
        if (err) LLVMDisposeMessage(err);
        rc = -1;
    }

    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);
    return rc;
}

void codegen_free(CodeGen* cg) {
    if (!cg) return;
    while (cg->scope) pop_scope(cg);
    CGFunc* f = cg->funcs;
    while (f) { CGFunc* next = f->next; free(f); f = next; }
    LLVMDisposeBuilder(cg->builder);
    LLVMDisposeModule(cg->mod);
    LLVMContextDispose(cg->ctx);
    free(cg);
}

