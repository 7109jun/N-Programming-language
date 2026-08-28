/*
 * N Language JIT Compiler (n-jit.c) - COMPLETE SINGLE FILE v3.0
 * 
 * 철학:
 * - Space is the dot (공백으로 객체/함수 접근)
 * - 인라인 주석 금지 (#은 줄 시작에서만)
 * - 연산자 양옆 공백 필수
 * - .txt 파일 내 <N Language = UTF8> 태그 실
 * - 다운로드 9~17초 윈도우 백그라운드 감시
 * - 화면 정중앙 콘솔 팝업 (X 버튼으로 종료)
 * - 단일 파일, 외부 라이브러리 Zero (표준 C 라이브러리 제외)
 * - 저수준 제어: unsafe, alloc, dealloc, ptr @ offset
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ==================== OS 추상화 및 헤더 ==================== */

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#define PATH_SEP '\\'
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <sys/mman.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <pthread.h>
#include <pwd.h>
#define PATH_SEP '/'
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ==================== 전역 상태 및 런타임 ==================== */

#define OUTPUT_BUF_SIZE 4096
static char g_output_buffer[OUTPUT_BUF_SIZE];
static size_t g_output_len = 0;

#define MAX_STRINGS 64
static char* g_string_pool[MAX_STRINGS];
static int g_string_count = 0;

int add_string(const char* s) {
    for(int i=0; i<g_string_count; i++) if(strcmp(g_string_pool[i], s)==0) return i;
    g_string_pool[g_string_count] = strdup(s);
    return g_string_count++;
}

void n_core_print_int(int64_t val) {
    char temp[64];
    int len = snprintf(temp, sizeof(temp), "%lld\n", (long long)val);
    if (g_output_len + len < OUTPUT_BUF_SIZE - 1) {
        memcpy(g_output_buffer + g_output_len, temp, len);
        g_output_len += len;
        g_output_buffer[g_output_len] = '\0';
    }
}

void n_core_print_str(const char* str) {
    size_t len = strlen(str);
    if (g_output_len + len + 1 < OUTPUT_BUF_SIZE - 1) {
        memcpy(g_output_buffer + g_output_len, str, len);
        g_output_buffer[g_output_len + len] = '\n';
        g_output_len += len + 1;
        g_output_buffer[g_output_len] = '\0';
    }
}

/* ==================== JIT 메모리 관리 ==================== */

typedef struct { void* ptr; size_t size; } ExecMemory;

ExecMemory* exec_alloc(size_t size) {
    ExecMemory* mem = malloc(sizeof(ExecMemory));
    if (!mem) return NULL;
#ifdef _WIN32
    mem->ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    mem->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem->ptr == MAP_FAILED) mem->ptr = NULL;
#endif
    mem->size = (mem->ptr) ? size : 0;
    return mem;
}

void exec_free(ExecMemory* mem) {
    if (!mem) return;
#ifdef _WIN32
    if (mem->ptr) VirtualFree(mem->ptr, 0, MEM_RELEASE);
#else
    if (mem->ptr && mem->ptr != MAP_FAILED) munmap(mem->ptr, mem->size);
#endif
    free(mem);
}

/* ==================== JIT 코드 생성 유틸리티 ==================== */

typedef struct { uint8_t* code; size_t size; size_t capacity; } JITBuffer;

JITBuffer* jit_buffer_create(size_t initial_size) {
    JITBuffer* buf = malloc(sizeof(JITBuffer));
    buf->code = malloc(initial_size);
    buf->size = 0; buf->capacity = initial_size;
    return buf;
}

void jit_emit(JITBuffer* buf, uint8_t byte) {
    if (buf->size >= buf->capacity) { buf->capacity *= 2; buf->code = realloc(buf->code, buf->capacity); }
    buf->code[buf->size++] = byte;
}

void jit_emit_int32(JITBuffer* buf, int32_t val) {
    uint8_t* p = (uint8_t*)&val;
    for (int i = 0; i < 4; i++) jit_emit(buf, p[i]);
}

void jit_emit_jmp(JITBuffer* buf) { jit_emit(buf, 0xE9); jit_emit_int32(buf, 0); }
void jit_emit_jz(JITBuffer* buf) { jit_emit(buf, 0x0F); jit_emit(buf, 0x84); jit_emit_int32(buf, 0); }

void jit_patch_jump(JITBuffer* buf, size_t jump_offset) {
    int32_t target = (int32_t)(buf->size - (jump_offset + 4));
    uint8_t* p = (uint8_t*)&target;
    for (int i = 0; i < 4; i++) buf->code[jump_offset + i] = p[i];
}

/* ==================== Lexer ==================== */

typedef enum {
    TOK_EOF, TOK_LET, TOK_PRINT, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FN, TOK_RETURN,
    TOK_UNSAFE, TOK_ALLOC, TOK_DEALLOC,
    TOK_IDENT, TOK_INT, TOK_STR,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_ASSIGN, TOK_EQ, TOK_LT, TOK_GT,
    TOK_COLON, TOK_AT
} TokenType;

typedef struct { TokenType type; union { int64_t int_val; char* str_val; } data; } Token;
typedef struct { const char* src; size_t pos; size_t len; } Lexer;

Lexer* lexer_create(const char* src) {
    Lexer* lex = malloc(sizeof(Lexer));
    lex->src = src; lex->pos = 0; lex->len = strlen(src);
    return lex;
}

char lexer_peek(Lexer* lex) { return (lex->pos < lex->len) ? lex->src[lex->pos] : '\0'; }
char lexer_advance(Lexer* lex) { return (lex->pos < lex->len) ? lex->src[lex->pos++] : '\0'; }

void lexer_skip_whitespace(Lexer* lex) {
    while (1) {
        char c = lexer_peek(lex);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') lexer_advance(lex);
        else if (c == '#') { while (lexer_peek(lex) != '\n' && lexer_peek(lex) != '\0') lexer_advance(lex); }
        else break;
    }
}

Token lexer_next_token(Lexer* lex) {
    Token tok = {TOK_EOF, {0}};
    lexer_skip_whitespace(lex);
    char c = lexer_peek(lex);
    if (c == '\0') return tok;

    if (c == '+') { tok.type = TOK_PLUS; lexer_advance(lex); return tok; }
    if (c == '-') { tok.type = TOK_MINUS; lexer_advance(lex); return tok; }
    if (c == '*') { tok.type = TOK_STAR; lexer_advance(lex); return tok; }
    if (c == '/') { tok.type = TOK_SLASH; lexer_advance(lex); return tok; }
    if (c == ':') { tok.type = TOK_COLON; lexer_advance(lex); return tok; }
    if (c == '@') { tok.type = TOK_AT; lexer_advance(lex); return tok; }
    if (c == '<') { tok.type = TOK_LT; lexer_advance(lex); return tok; }
    if (c == '>') { tok.type = TOK_GT; lexer_advance(lex); return tok; }
    
    if (c == '=') {
        lexer_advance(lex);
        if (lexer_peek(lex) == '=') { lexer_advance(lex); tok.type = TOK_EQ; }
        else { tok.type = TOK_ASSIGN; }
        return tok;
    }

    if (c == '"') {
        lexer_advance(lex);
        size_t start = lex->pos;
        while (lexer_peek(lex) != '"' && lexer_peek(lex) != '\0') lexer_advance(lex);
        size_t len = lex->pos - start;
        tok.type = TOK_STR;
        tok.data.str_val = malloc(len + 1);
        strncpy(tok.data.str_val, lex->src + start, len);
        tok.data.str_val[len] = '\0';
        if (lexer_peek(lex) == '"') lexer_advance(lex);
        return tok;
    }

    if (c >= '0' && c <= '9') {
        int64_t val = 0;
        while (lexer_peek(lex) >= '0' && lexer_peek(lex) <= '9') val = val * 10 + (lexer_advance(lex) - '0');
        tok.type = TOK_INT; tok.data.int_val = val;
        return tok;
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        size_t start = lex->pos;
        while (1) {
            char ch = lexer_peek(lex);
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') lexer_advance(lex);
            else break;
        }
        size_t len = lex->pos - start;
        char* ident = malloc(len + 1);
        strncpy(ident, lex->src + start, len); ident[len] = '\0';

        if (strcmp(ident, "let") == 0) tok.type = TOK_LET;
        else if (strcmp(ident, "print") == 0) tok.type = TOK_PRINT;
        else if (strcmp(ident, "if") == 0) tok.type = TOK_IF;
        else if (strcmp(ident, "else") == 0) tok.type = TOK_ELSE;
        else if (strcmp(ident, "while") == 0) tok.type = TOK_WHILE;
        else if (strcmp(ident, "fn") == 0) tok.type = TOK_FN;
        else if (strcmp(ident, "return") == 0) tok.type = TOK_RETURN;
        else if (strcmp(ident, "unsafe") == 0) tok.type = TOK_UNSAFE;
        else if (strcmp(ident, "alloc") == 0) tok.type = TOK_ALLOC;
        else if (strcmp(ident, "dealloc") == 0) tok.type = TOK_DEALLOC;
        else { tok.type = TOK_IDENT; tok.data.str_val = ident; return tok; }
        free(ident);
        return tok;
    }
    lexer_advance(lex);
    return tok;
}

/* ==================== Parser (AST) ==================== */

typedef enum { 
    NODE_INT, NODE_STR, NODE_IDENT, NODE_BINOP, NODE_VAR_DECL, NODE_VAR_ASSIGN,
    NODE_PRINT, NODE_BLOCK, NODE_IF, NODE_WHILE, NODE_FUNC_DEF, NODE_CALL, NODE_RETURN,
    NODE_UNSAFE, NODE_ALLOC, NODE_DEALLOC, NODE_PTR_LOAD, NODE_PTR_STORE
} NodeType;

typedef struct ASTNode {
    NodeType type;
    union {
        int64_t int_val; char* str_val; char* ident_name;
        struct { struct ASTNode* left; struct ASTNode* right; char op; } binop;
        struct { char* name; struct ASTNode* value; } var_decl;
        struct { char* name; struct ASTNode* value; } var_assign;
        struct { struct ASTNode* value; } print;
        struct { struct ASTNode** stmts; int count; } block;
        struct { struct ASTNode* cond; struct ASTNode* then_body; struct ASTNode* else_body; } if_stmt;
        struct { struct ASTNode* cond; struct ASTNode* body; } while_stmt;
        struct { char* name; char** params; int param_count; struct ASTNode* body; } func_def;
        struct { char* name; struct ASTNode** args; int arg_count; } call;
        struct { struct ASTNode* value; } ret;
        struct { struct ASTNode* body; } unsafe_block;
        struct { struct ASTNode* size; } alloc;
        struct { struct ASTNode* ptr; } dealloc;
        struct { char* name; struct ASTNode* offset; } ptr_load;
        struct { char* name; struct ASTNode* offset; struct ASTNode* value; } ptr_store;
    } data;
} ASTNode;

typedef struct { Lexer* lex; Token curr; Token peek; } Parser;

Parser* parser_create(Lexer* lex) {
    Parser* p = malloc(sizeof(Parser));
    p->lex = lex;
    p->curr = lexer_next_token(lex);
    p->peek = lexer_next_token(lex);
    return p;
}

void parser_advance(Parser* p) { p->curr = p->peek; p->peek = lexer_next_token(p->lex); }

ASTNode* ast_node(NodeType type) { ASTNode* n = calloc(1, sizeof(ASTNode)); n->type = type; return n; }

ASTNode* parse_expr(Parser* p);
ASTNode* parse_block(Parser* p);

ASTNode* parse_primary(Parser* p) {
    if (p->curr.type == TOK_INT) {
        ASTNode* n = ast_node(NODE_INT); n->data.int_val = p->curr.data.int_val; parser_advance(p); return n;
    }
    if (p->curr.type == TOK_STR) {
        ASTNode* n = ast_node(NODE_STR); n->data.str_val = p->curr.data.str_val; parser_advance(p); return n;
    }
    if (p->curr.type == TOK_ALLOC) {
        parser_advance(p);
        ASTNode* n = ast_node(NODE_ALLOC); n->data.alloc.size = parse_expr(p); return n;
    }
    if (p->curr.type == TOK_IDENT) {
        // Pointer load: ptr @ offset
        if (p->peek.type == TOK_AT) {
            char* name = p->curr.data.str_val;
            parser_advance(p); parser_advance(p); // ident, @
            ASTNode* offset = parse_expr(p);
            ASTNode* n = ast_node(NODE_PTR_LOAD);
            n->data.ptr_load.name = name; n->data.ptr_load.offset = offset;
            return n;
        }
        // Function call: name arg1 arg2
        if (p->peek.type == TOK_INT || p->peek.type == TOK_STR || p->peek.type == TOK_IDENT) {
            char* name = p->curr.data.str_val;
            parser_advance(p);
            ASTNode** args = NULL; int arg_count = 0;
            while (p->curr.type == TOK_INT || p->curr.type == TOK_STR || p->curr.type == TOK_IDENT) {
                args = realloc(args, sizeof(ASTNode*) * (arg_count + 1));
                args[arg_count++] = parse_primary(p);
            }
            ASTNode* n = ast_node(NODE_CALL);
            n->data.call.name = name; n->data.call.args = args; n->data.call.arg_count = arg_count;
            return n;
        }
        ASTNode* n = ast_node(NODE_IDENT); n->data.ident_name = p->curr.data.str_val; parser_advance(p); return n;
    }
    return ast_node(NODE_INT);
}

ASTNode* parse_multiplicative(Parser* p) {
    ASTNode* left = parse_primary(p);
    while (p->curr.type == TOK_STAR || p->curr.type == TOK_SLASH) {
        char op = (p->curr.type == TOK_STAR) ? '*' : '/';
        parser_advance(p);
        ASTNode* n = ast_node(NODE_BINOP); n->data.binop.op = op; n->data.binop.left = left; n->data.binop.right = parse_primary(p); left = n;
    }
    return left;
}

ASTNode* parse_additive(Parser* p) {
    ASTNode* left = parse_multiplicative(p);
    while (p->curr.type == TOK_PLUS || p->curr.type == TOK_MINUS) {
        char op = (p->curr.type == TOK_PLUS) ? '+' : '-';
        parser_advance(p);
        ASTNode* n = ast_node(NODE_BINOP); n->data.binop.op = op; n->data.binop.left = left; n->data.binop.right = parse_multiplicative(p); left = n;
    }
    return left;
}

ASTNode* parse_comparison(Parser* p) {
    ASTNode* left = parse_additive(p);
    while (p->curr.type == TOK_EQ || p->curr.type == TOK_LT || p->curr.type == TOK_GT) {
        char op = 0;
        if (p->curr.type == TOK_EQ) op = '=';
        else if (p->curr.type == TOK_LT) op = '<';
        else if (p->curr.type == TOK_GT) op = '>';
        parser_advance(p);
        ASTNode* n = ast_node(NODE_BINOP); n->data.binop.op = op; n->data.binop.left = left; n->data.binop.right = parse_additive(p); left = n;
    }
    return left;
}

ASTNode* parse_expr(Parser* p) { return parse_comparison(p); }

ASTNode* parse_statement(Parser* p) {
    if (p->curr.type == TOK_LET) {
        parser_advance(p);
        char* name = p->curr.data.str_val; parser_advance(p);
        if (p->curr.type == TOK_COLON) { while (p->curr.type != TOK_ASSIGN) parser_advance(p); }
        parser_advance(p);
        ASTNode* n = ast_node(NODE_VAR_DECL); n->data.var_decl.name = name; n->data.var_decl.value = parse_expr(p);
        return n;
    }
    if (p->curr.type == TOK_IDENT && p->peek.type == TOK_ASSIGN) {
        char* name = p->curr.data.str_val; parser_advance(p); parser_advance(p);
        ASTNode* n = ast_node(NODE_VAR_ASSIGN); n->data.var_assign.name = name; n->data.var_assign.value = parse_expr(p);
        return n;
    }
    // Pointer store: ptr @ offset = value
    if (p->curr.type == TOK_IDENT && p->peek.type == TOK_AT) {
        char* name = p->curr.data.str_val;
        parser_advance(p); parser_advance(p); // ident, @
        ASTNode* offset = parse_expr(p);
        if (p->curr.type == TOK_ASSIGN) {
            parser_advance(p);
            ASTNode* value = parse_expr(p);
            ASTNode* n = ast_node(NODE_PTR_STORE);
            n->data.ptr_store.name = name; n->data.ptr_store.offset = offset; n->data.ptr_store.value = value;
            return n;
        }
    }
    if (p->curr.type == TOK_PRINT) {
        parser_advance(p);
        ASTNode* n = ast_node(NODE_PRINT); n->data.print.value = parse_expr(p);
        return n;
    }
    if (p->curr.type == TOK_DEALLOC) {
        parser_advance(p);
        ASTNode* n = ast_node(NODE_DEALLOC); n->data.dealloc.ptr = parse_expr(p);
        return n;
    }
    if (p->curr.type == TOK_IF) {
        parser_advance(p);
        ASTNode* cond = parse_expr(p);
        if (p->curr.type == TOK_COLON) parser_advance(p);
        ASTNode* then_body = parse_block(p);
        ASTNode* else_body = NULL;
        if (p->curr.type == TOK_ELSE) {
            parser_advance(p);
            if (p->curr.type == TOK_COLON) parser_advance(p);
            else_body = parse_block(p);
        }
        ASTNode* n = ast_node(NODE_IF);
        n->data.if_stmt.cond = cond; n->data.if_stmt.then_body = then_body; n->data.if_stmt.else_body = else_body;
        return n;
    }
    if (p->curr.type == TOK_WHILE) {
        parser_advance(p);
        ASTNode* cond = parse_expr(p);
        if (p->curr.type == TOK_COLON) parser_advance(p);
        ASTNode* body = parse_block(p);
        ASTNode* n = ast_node(NODE_WHILE); n->data.while_stmt.cond = cond; n->data.while_stmt.body = body;
        return n;
    }
    if (p->curr.type == TOK_UNSAFE) {
        parser_advance(p);
        if (p->curr.type == TOK_COLON) parser_advance(p);
        ASTNode* body = parse_block(p);
        ASTNode* n = ast_node(NODE_UNSAFE); n->data.unsafe_block.body = body;
        return n;
    }
    if (p->curr.type == TOK_FN) {
        parser_advance(p);
        char* name = p->curr.data.str_val; parser_advance(p);
        char** params = NULL; int param_count = 0;
        while (p->curr.type != TOK_COLON && p->curr.type != TOK_EOF) {
            if (p->curr.type == TOK_IDENT) {
                params = realloc(params, sizeof(char*) * (param_count + 1));
                params[param_count++] = p->curr.data.str_val;
            }
            parser_advance(p);
        }
        if (p->curr.type == TOK_COLON) parser_advance(p);
        ASTNode* body = parse_block(p);
        ASTNode* n = ast_node(NODE_FUNC_DEF);
        n->data.func_def.name = name; n->data.func_def.params = params; n->data.func_def.param_count = param_count; n->data.func_def.body = body;
        return n;
    }
    if (p->curr.type == TOK_RETURN) {
        parser_advance(p);
        ASTNode* n = ast_node(NODE_RETURN); n->data.ret.value = parse_expr(p);
        return n;
    }
    return parse_expr(p);
}

ASTNode* parse_block(Parser* p) {
    ASTNode** stmts = NULL; int count = 0;
    while (p->curr.type != TOK_EOF) {
        ASTNode* s = parse_statement(p);
        stmts = realloc(stmts, sizeof(ASTNode*) * (count + 1)); stmts[count++] = s;
    }
    ASTNode* n = ast_node(NODE_BLOCK); n->data.block.stmts = stmts; n->data.block.count = count;
    return n;
}

/* ==================== JIT Code Generator ==================== */

#define MAX_VARS 64
typedef struct { char name[64]; int offset; } VarInfo;
static VarInfo g_vars[MAX_VARS]; static int g_var_count = 0; static int g_stack_off = 0;

#define MAX_FUNCS 16
typedef struct { char name[64]; void* code; int param_count; } FuncInfo;
static FuncInfo g_funcs[MAX_FUNCS]; static int g_func_count = 0;

int get_var(const char* name) {
    for (int i = 0; i < g_var_count; i++) if (strcmp(g_vars[i].name, name) == 0) return g_vars[i].offset;
    g_stack_off += 8; strcpy(g_vars[g_var_count].name, name); g_vars[g_var_count].offset = g_stack_off;
    return g_vars[g_var_count++].offset;
}

void gen_expr(JITBuffer* buf, ASTNode* node);
void gen_stmt(JITBuffer* buf, ASTNode* node);

void gen_expr(JITBuffer* buf, ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_INT) {
        jit_emit(buf, 0x48); jit_emit(buf, 0xB8); jit_emit_int32(buf, (int32_t)node->data.int_val); jit_emit_int32(buf, (int32_t)(node->data.int_val >> 32));
    } else if (node->type == NODE_STR) {
        int idx = add_string(node->data.str_val);
        uint64_t addr = (uint64_t)g_string_pool[idx];
        jit_emit(buf, 0x48); jit_emit(buf, 0xB8);
        for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
    } else if (node->type == NODE_IDENT) {
        int off = get_var(node->data.ident_name);
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off));
    } else if (node->type == NODE_ALLOC) {
        gen_expr(buf, node->data.alloc.size);
#ifdef _WIN32
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC1); // mov rcx, rax
#else
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC7); // mov rdi, rax
#endif
        jit_emit(buf, 0x48); jit_emit(buf, 0xB8);
        uint64_t addr = (uint64_t)malloc;
        for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
        jit_emit(buf, 0xFF); jit_emit(buf, 0xD0); // call rax
    } else if (node->type == NODE_PTR_LOAD) {
        int off = get_var(node->data.ptr_load.name);
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off)); // mov rax, [ptr]
        gen_expr(buf, node->data.ptr_load.offset); // offset to rcx
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC1); // mov rcx, rax
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off)); // mov rax, [ptr]
        jit_emit(buf, 0x48); jit_emit(buf, 0x01); jit_emit(buf, 0xC8); // add rax, rcx
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x00); // mov rax, [rax]
    } else if (node->type == NODE_CALL) {
        void* func_addr = NULL;
        for(int i=0; i<g_func_count; i++) {
            if(strcmp(g_funcs[i].name, node->data.call.name) == 0) { func_addr = g_funcs[i].code; break; }
        }
        if (!func_addr) return;
        for (int i = node->data.call.arg_count - 1; i >= 0; i--) {
            gen_expr(buf, node->data.call.args[i]);
            jit_emit(buf, 0x50); // push rax
        }
        jit_emit(buf, 0x48); jit_emit(buf, 0xB8);
        uint64_t addr = (uint64_t)func_addr;
        for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
        jit_emit(buf, 0xFF); jit_emit(buf, 0xD0);
        if (node->data.call.arg_count > 0) {
            jit_emit(buf, 0x48); jit_emit(buf, 0x83); jit_emit(buf, 0xC4); jit_emit(buf, (uint8_t)(node->data.call.arg_count * 8));
        }
    } else if (node->type == NODE_BINOP) {
        gen_expr(buf, node->data.binop.left); jit_emit(buf, 0x50);
        gen_expr(buf, node->data.binop.right); jit_emit(buf, 0x59);
        jit_emit(buf, 0x52); jit_emit(buf, 0x58);
        jit_emit(buf, 0x48); jit_emit(buf, 0x87); jit_emit(buf, 0xC1);
        char op = node->data.binop.op;
        if (op == '+' || op == '-' || op == '*' || op == '/') {
            if (op == '+') { jit_emit(buf, 0x48); jit_emit(buf, 0x01); jit_emit(buf, 0xC8); }
            else if (op == '-') { jit_emit(buf, 0x48); jit_emit(buf, 0x29); jit_emit(buf, 0xC8); }
            else if (op == '*') { jit_emit(buf, 0x48); jit_emit(buf, 0x0F); jit_emit(buf, 0xAF); jit_emit(buf, 0xC1); }
        } else {
            jit_emit(buf, 0x48); jit_emit(buf, 0x39); jit_emit(buf, 0xC1);
            if (op == '=') { jit_emit(buf, 0x0F); jit_emit(buf, 0x94); jit_emit(buf, 0xC0); }
            else if (op == '<') { jit_emit(buf, 0x0F); jit_emit(buf, 0x9C); jit_emit(buf, 0xC0); }
            else if (op == '>') { jit_emit(buf, 0x0F); jit_emit(buf, 0x9F); jit_emit(buf, 0xC0); }
            jit_emit(buf, 0x48); jit_emit(buf, 0x0F); jit_emit(buf, 0xB6); jit_emit(buf, 0xC0);
        }
    }
}

void gen_stmt(JITBuffer* buf, ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_VAR_DECL) {
        gen_expr(buf, node->data.var_decl.value);
        int off = get_var(node->data.var_decl.name);
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off));
    } else if (node->type == NODE_VAR_ASSIGN) {
        gen_expr(buf, node->data.var_assign.value);
        int off = get_var(node->data.var_assign.name);
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off));
    } else if (node->type == NODE_PTR_STORE) {
        int off = get_var(node->data.ptr_store.name);
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off)); // mov rax, [ptr]
        gen_expr(buf, node->data.ptr_store.offset); // offset to rcx
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC1); // mov rcx, rax
        jit_emit(buf, 0x48); jit_emit(buf, 0x8B); jit_emit(buf, 0x45); jit_emit(buf, (uint8_t)(-off)); // mov rax, [ptr]
        jit_emit(buf, 0x48); jit_emit(buf, 0x01); jit_emit(buf, 0xC8); // add rax, rcx (rax = target addr)
        gen_expr(buf, node->data.ptr_store.value); // value to rcx
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0x08); // mov [rax], rcx
    } else if (node->type == NODE_DEALLOC) {
        gen_expr(buf, node->data.dealloc.ptr);
#ifdef _WIN32
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC1); // mov rcx, rax
#else
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC7); // mov rdi, rax
#endif
        jit_emit(buf, 0x48); jit_emit(buf, 0xB8);
        uint64_t addr = (uint64_t)free;
        for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
        jit_emit(buf, 0xFF); jit_emit(buf, 0xD0);
    } else if (node->type == NODE_PRINT) {
        gen_expr(buf, node->data.print.value);
        if (node->data.print.value->type == NODE_STR) {
            jit_emit(buf, 0x48); jit_emit(buf, 0xB8); 
            uint64_t addr = (uint64_t)n_core_print_str;
            for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
        } else {
            jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xC7);
            jit_emit(buf, 0x48); jit_emit(buf, 0xB8); 
            uint64_t addr = (uint64_t)n_core_print_int;
            for(int i=0;i<8;i++) jit_emit(buf, ((uint8_t*)&addr)[i]);
        }
        jit_emit(buf, 0xFF); jit_emit(buf, 0xD0);
    } else if (node->type == NODE_IF) {
        gen_expr(buf, node->data.if_stmt.cond);
        jit_emit(buf, 0x48); jit_emit(buf, 0x85); jit_emit(buf, 0xC0);
        size_t jz_off = buf->size; jit_emit_jz(buf);
        gen_stmt(buf, node->data.if_stmt.then_body);
        if (node->data.if_stmt.else_body) {
            size_t jmp_off = buf->size; jit_emit_jmp(buf);
            jit_patch_jump(buf, jz_off);
            gen_stmt(buf, node->data.if_stmt.else_body);
            jit_patch_jump(buf, jmp_off);
        } else { jit_patch_jump(buf, jz_off); }
    } else if (node->type == NODE_WHILE) {
        size_t loop_start = buf->size;
        gen_expr(buf, node->data.while_stmt.cond);
        jit_emit(buf, 0x48); jit_emit(buf, 0x85); jit_emit(buf, 0xC0);
        size_t jz_off = buf->size; jit_emit_jz(buf);
        gen_stmt(buf, node->data.while_stmt.body);
        int32_t back = (int32_t)(loop_start - (buf->size + 5));
        jit_emit(buf, 0xE9); jit_emit_int32(buf, back);
        jit_patch_jump(buf, jz_off);
    } else if (node->type == NODE_UNSAFE) {
        gen_stmt(buf, node->data.unsafe_block.body);
    } else if (node->type == NODE_RETURN) {
        if (node->data.ret.value) gen_expr(buf, node->data.ret.value);
        jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xEC);
        jit_emit(buf, 0x5D); jit_emit(buf, 0xC3);
    } else if (node->type == NODE_FUNC_DEF) {
        JITBuffer* func_buf = jit_buffer_create(4096);
        jit_emit(func_buf, 0x55); jit_emit(func_buf, 0x48); jit_emit(func_buf, 0x89); jit_emit(func_buf, 0xE5);
        jit_emit(func_buf, 0x48); jit_emit(func_buf, 0x81); jit_emit(func_buf, 0xEC); jit_emit_int32(func_buf, 256);
        
        int prev_var_count = g_var_count; int prev_stack_off = g_stack_off;
        for(int i=0; i<node->data.func_def.param_count; i++) {
            int off = 16 + (i * 8);
            strcpy(g_vars[g_var_count].name, node->data.func_def.params[i]);
            g_vars[g_var_count].offset = off;
            g_var_count++;
        }
        
        gen_stmt(func_buf, node->data.func_def.body);
        
        jit_emit(func_buf, 0x48); jit_emit(func_buf, 0x31); jit_emit(func_buf, 0xC0);
        jit_emit(func_buf, 0x48); jit_emit(func_buf, 0x89); jit_emit(func_buf, 0xEC);
        jit_emit(func_buf, 0x5D); jit_emit(func_buf, 0xC3);
        
        ExecMemory* mem = exec_alloc(func_buf->size);
        memcpy(mem->ptr, func_buf->code, func_buf->size);
        
        strcpy(g_funcs[g_func_count].name, node->data.func_def.name);
        g_funcs[g_func_count].code = mem->ptr;
        g_funcs[g_func_count].param_count = node->data.func_def.param_count;
        g_func_count++;
        
        g_var_count = prev_var_count; g_stack_off = prev_stack_off;
        free(func_buf->code); free(func_buf);
    } else if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->data.block.count; i++) gen_stmt(buf, node->data.block.stmts[i]);
    }
}

/* ==================== 팝업 UI ==================== */

#ifdef _WIN32
LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        SetTextColor(hdc, RGB(0, 255, 0)); SetBkColor(hdc, RGB(0, 0, 0));
        TextOutA(hdc, 20, 20, g_output_buffer, strlen(g_output_buffer));
        EndPaint(hwnd, &ps);
    } else if (msg == WM_CLOSE) { DestroyWindow(hwnd); PostQuitMessage(0); }
    else return DefWindowProcA(hwnd, msg, wp, lp);
    return 0;
}

void show_popup() {
    WNDCLASSA wc = {0}; wc.lpfnWndProc = PopupProc; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "NPop"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    int w = 400, h = 300, x = (GetSystemMetrics(SM_CXSCREEN)-w)/2, y = (GetSystemMetrics(SM_CYSCREEN)-h)/2;
    CreateWindowExA(0, "NPop", "N Language", WS_OVERLAPPEDWINDOW|WS_VISIBLE, x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
}
#else
void show_popup() {
    printf("\n========== N Output ==========\n%s\n==============================\n[Enter to close]", g_output_buffer);
    getchar();
}
#endif

/* ==================== 파일 추출 및 실행 ==================== */

char* extract_n_code(const char* content) {
    const char* s = strstr(content, "<N Language = UTF8>");
    if (!s) return NULL; s += strlen("<N Language = UTF8>");
    const char* e = strstr(s, "<N Language end = UTF8>");
    if (!e) return NULL;
    size_t len = e - s; char* code = malloc(len + 1); strncpy(code, s, len); code[len] = '\0';
    return code;
}

void run_n_code(const char* code) {
    g_output_len = 0; g_output_buffer[0] = '\0'; g_var_count = 0; g_stack_off = 0; g_string_count = 0; g_func_count = 0;
    
    Lexer* lex = lexer_create(code);
    Parser* parser = parser_create(lex);
    ASTNode* ast = parse_block(parser);
    
    JITBuffer* buf = jit_buffer_create(4096);
    jit_emit(buf, 0x55); jit_emit(buf, 0x48); jit_emit(buf, 0x89); jit_emit(buf, 0xE5);
    jit_emit(buf, 0x48); jit_emit(buf, 0x81); jit_emit(buf, 0xEC); jit_emit_int32(buf, 256);
    
    gen_stmt(buf, ast);
    
    jit_emit(buf, 0x48); jit_emit(buf, 0x81); jit_emit(buf, 0xC4); jit_emit_int32(buf, 256);
    jit_emit(buf, 0x5D); jit_emit(buf, 0xC3);
    
    ExecMemory* mem = exec_alloc(buf->size);
    memcpy(mem->ptr, buf->code, buf->size);
    ((void(*)())mem->ptr)();
    
    show_popup();
    
    exec_free(mem); free(buf->code); free(buf); free(lex); free(parser);
}

/* ==================== 다운로드 폴더 감시 (9~17초 윈도우) ==================== */

#ifdef _WIN32
void get_download_folder(char* path, size_t size) {
    SHGetFolderPathA(NULL, CSIDL_DOWNLOADS, NULL, 0, path);
}

void watch_downloads() {
    char download_path[MAX_PATH];
    get_download_folder(download_path, sizeof(download_path));
    printf("Watching: %s\n", download_path);
    
    HANDLE hDir = CreateFileA(download_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hDir == INVALID_HANDLE_VALUE) return;
    
    char buffer[4096]; DWORD bytes;
    while (1) {
        if (!ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), FALSE, FILE_ACTION_ADDED, &bytes, NULL, NULL)) { SLEEP_MS(1000); continue; }
        FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)buffer;
        if (fni->Action == FILE_ACTION_ADDED) {
            wchar_t* ext = wcschr(fni->FileName, L'.');
            if (ext && _wcsicmp(ext, L".txt") == 0) {
                SLEEP_MS(9000);
                wchar_t full_path[MAX_PATH];
                swprintf(full_path, MAX_PATH, L"%s\\%s", download_path, fni->FileName);
                HANDLE hFile = CreateFileW(full_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD size = GetFileSize(hFile, NULL);
                    char* content = malloc(size + 1); DWORD read;
                    ReadFile(hFile, content, size, &read, NULL); content[size] = '\0'; CloseHandle(hFile);
                    char* n_code = extract_n_code(content);
                    if (n_code) { run_n_code(n_code); free(n_code); }
                    free(content);
                }
                SLEEP_MS(8000);
            }
        }
    }
}
#else
void get_download_folder(char* path, size_t size) {
    const char* home = getenv("HOME");
    if (!home) { struct passwd* pw = getpwuid(getuid()); home = pw->pw_dir; }
    snprintf(path, size, "%s/Downloads", home);
}

void watch_downloads() {
    char download_path[512];
    get_download_folder(download_path, sizeof(download_path));
    printf("Watching: %s\n", download_path);
    
    int fd = inotify_init();
    int wd = inotify_add_watch(fd, download_path, IN_CREATE);
    if (wd < 0) return;
    
    char buffer[4096];
    while (1) {
        int len = read(fd, buffer, sizeof(buffer));
        if (len > 0) {
            int i = 0;
            while (i < len) {
                struct inotify_event* event = (struct inotify_event*)&buffer[i];
                if (event->mask & IN_CREATE) {
                    size_t name_len = strlen(event->name);
                    if (name_len > 4 && strcmp(event->name + name_len - 4, ".txt") == 0) {
                        SLEEP_MS(9000);
                        char path[1024]; snprintf(path, sizeof(path), "%s/%s", download_path, event->name);
                        FILE* f = fopen(path, "r");
                        if (f) {
                            fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
                            char* content = malloc(size + 1); fread(content, 1, size, f); content[size] = '\0'; fclose(f);
                            char* n_code = extract_n_code(content);
                            if (n_code) { run_n_code(n_code); free(n_code); }
                            free(content);
                        }
                        SLEEP_MS(8000);
                    }
                }
                i += sizeof(struct inotify_event) + event->len;
            }
        }
    }
    inotify_rm_watch(fd, wd); close(fd);
}
#endif

/* ==================== 메인 ==================== */

int main(int argc, char* argv[]) {
    if (argc > 1) {
        FILE* f = fopen(argv[1], "r");
        if (!f) { printf("Error: Cannot open file %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char* content = malloc(sz + 1); fread(content, 1, sz, f); content[sz] = '\0'; fclose(f);
        
        char* code = strstr(argv[1], ".txt") ? extract_n_code(content) : content;
        if (code) run_n_code(code);
        else printf("No N code found\n");
        free(content); if (code != content) free(code);
    } else {
        printf("N Watcher Daemon Started\nMonitoring downloads (9-17 second window)...\n");
        watch_downloads();
    }
    return 0;
}
