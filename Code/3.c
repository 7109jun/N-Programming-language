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

Token lexer
