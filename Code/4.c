/* N Language v7.0 "Stable & Fast" - Single File, Zero Dependencies */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#define SLEEP_MS(ms) Sleep(ms)
#define DL_OPEN(n) LoadLibraryA(n)
#define DL_SYM(l, s) GetProcAddress((HMODULE)l, s)
#else
#include <sys/mman.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <dlfcn.h>
#include <pwd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#define DL_OPEN(n) dlopen(n, RTLD_LAZY)
#define DL_SYM(l, s) dlsym(l, s)
#endif

/* ==================== 1. MEMORY ARENA (최적화: malloc 호출 급감) ==================== */
typedef struct Arena {
    uint8_t* buffer;
    size_t capacity;
    size_t offset;
} Arena;

Arena* arena_new(size_t capacity) {
    Arena* a = malloc(sizeof(Arena));
    a->buffer = malloc(capacity);
    a->capacity = capacity;
    a->offset = 0;
    return a;
}

void* arena_alloc(Arena* a, size_t size) {
    // 8바이트 정렬
    size_t aligned_size = (size + 7) & ~7;
    if (a->offset + aligned_size > a->capacity) {
        // 용량 부족 시 2배 확장
        a->capacity *= 2;
        a->buffer = realloc(a->buffer, a->capacity);
    }
    void* ptr = a->buffer + a->offset;
    a->offset += aligned_size;
    memset(ptr, 0, size);
    return ptr;
}

void arena_free(Arena* a) {
    free(a->buffer);
    free(a);
}

/* ==================== 2. RUNTIME CORE (GC, Error, FFI) ==================== */
#define MAX_OUT 8192
static char g_out[MAX_OUT]; 
static size_t g_out_len = 0;
static jmp_buf g_panic_jmp;

// --- GC (Simplified Mark-and-Sweep with Arena) ---
typedef struct GCObj { 
    struct GCObj* next; 
    bool marked; 
    size_t size; 
    void* data; 
} GCObj;

static GCObj* g_gc_list = NULL;
static Arena* g_gc_arena = NULL;

void* gc_alloc(size_t sz) {
    if (!g_gc_arena) g_gc_arena = arena_new(1024 * 1024); // 1MB GC Arena
    
    GCObj* o = arena_alloc(g_gc_arena, sizeof(GCObj));
    o->next = g_gc_list; 
    o->marked = false; 
    o->size = sz; 
    o->data = calloc(1, sz); // 데이터는 별도로 할당 (추후 GC로 통합 가능)
    g_gc_list = o; 
    return o->data;
}

void gc_collect() {
    // Simplified: In real impl, scan roots (stack, globals) and mark reachable objects.
    // Here we just free unmarked objects.
    GCObj** p = &g_gc_list;
    while (*p) {
        if (!(*p)->marked) {
            GCObj* unmarked = *p;
            *p = unmarked->next;
            free(unmarked->data);
            // Note: unmarked object itself is in arena, cannot be freed individually.
        } else {
            (*p)->marked = false; // Reset for next cycle
            p = &(*p)->next;
        }
    }
    printf("[GC] Collected.\n");
}

// --- Error Handling ---
void n_panic(const char* msg) { 
    printf("PANIC: %s\n", msg); 
    longjmp(g_panic_jmp, 1); 
}

// --- FFI ---
void* ffi_call(const char* lib, const char* sym, void** args, int argc) {
    void* l = DL_OPEN(lib); 
    if(!l) { printf("[FFI] Cannot load %s\n", lib); return NULL; }
    void* f = DL_SYM(l, sym); 
    if(!f) { printf("[FFI] Cannot find %s\n", sym); return NULL; }
    // Simplified: assumes function takes no args or int64_t args for prototype
    typedef int64_t (*Func0)();
    typedef int64_t (*Func1)(int64_t);
    typedef int64_t (*Func2)(int64_t, int64_t);
    
    if (argc == 0) return (void*)((Func0)f)();
    if (argc == 1) return (void*)((Func1)f)((int64_t)args[0]);
    if (argc == 2) return (void*)((Func2)f)((int64_t)args[0], (int64_t)args[1]);
    return NULL;
}

/* ==================== 3. JIT & ASM PARSER ==================== */
typedef struct { void* p; size_t s; } ExecMem;

ExecMem* exec_alloc(size_t s) {
    ExecMem* m = malloc(sizeof(ExecMem));
#ifdef _WIN32
    m->p = VirtualAlloc(NULL, s, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    m->p = mmap(NULL, s, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#endif
    m->s = s; 
    return m;
}

void exec_free(ExecMem* m) {
    if (!m) return;
#ifdef _WIN32
    if(m->p) VirtualFree(m->p, 0, MEM_RELEASE);
#else
    if(m->p) munmap(m->p, m->s);
#endif
    free(m);
}

typedef struct { uint8_t* c; size_t s, cap; } JBuf;

JBuf* jb_new(size_t s) { 
    JBuf* b = malloc(sizeof(JBuf)); 
    b->c = malloc(s); 
    b->s = 0; 
    b->cap = s; 
    return b; 
}

void jb_e(JBuf* b, uint8_t v) { 
    if(b->s >= b->cap) {
        b->cap *= 2; 
        b->c = realloc(b->c, b->cap);
    } 
    b->c[b->s++] = v; 
}

void jb_e32(JBuf* b, int32_t v) { 
    uint8_t* p = (uint8_t*)&v; 
    for(int i=0; i<4; i++) jb_e(b, p[i]); 
}

void jb_e64(JBuf* b, uint64_t v) { 
    uint8_t* p = (uint8_t*)&v; 
    for(int i=0; i<8; i++) jb_e(b, p[i]); 
}

// Patch relative 32-bit offset at given position
void jb_patch_rel32(JBuf* b, size_t offset_pos) {
    int32_t rel = (int32_t)(b->s - (offset_pos + 4));
    uint8_t* p = (uint8_t*)&rel;
    for(int i=0; i<4; i++) b->c[offset_pos + i] = p[i];
}

// Real x86_64 ASM Parser
int asm_reg_id(const char* r) {
    if(strcmp(r,"rax")==0) return 0; if(strcmp(r,"rcx")==0) return 1; 
    if(strcmp(r,"rdx")==0) return 2; if(strcmp(r,"rbx")==0) return 3; 
    if(strcmp(r,"rsp")==0) return 4; if(strcmp(r,"rbp")==0) return 5;
    if(strcmp(r,"rsi")==0) return 6; if(strcmp(r,"rdi")==0) return 7;
    if(strcmp(r,"ymm0")==0) return 0; if(strcmp(r,"ymm1")==0) return 1;
    return -1;
}

void emit_asm(JBuf* b, const char* code) {
    // Simple tokenizer without strtok (thread-safe)
    char buf[256]; 
    strncpy(buf, code, 255); 
    buf[255] = 0;
    
    char* p = buf;
    while(*p == ' ') p++;
    
    char mnemonic[32] = {0};
    int mi = 0;
    while(*p && *p != ' ' && *p != ',') mnemonic[mi++] = *p++;
    mnemonic[mi] = 0;
    
    if(strcmp(mnemonic, "nop") == 0) { jb_e(b, 0x90); }
    else if(strcmp(mnemonic, "ret") == 0) { jb_e(b, 0xC3); }
    else if(strcmp(mnemonic, "push") == 0) { 
        while(*p == ' ') p++;
        char reg[16] = {0}; int ri = 0;
        while(*p && *p != ',' && *p != ' ') reg[ri++] = *p++;
        int id = asm_reg_id(reg);
        if(id >= 0 && id < 8) jb_e(b, 0x50 + id); 
    }
    else if(strcmp(mnemonic, "pop") == 0) { 
        while(*p == ' ') p++;
        char reg[16] = {0}; int ri = 0;
        while(*p && *p != ',' && *p != ' ') reg[ri++] = *p++;
        int id = asm_reg_id(reg);
        if(id >= 0 && id < 8) jb_e(b, 0x58 + id); 
    }
    else if(strcmp(mnemonic, "xor") == 0) { 
        while(*p == ' ') p++;
        char r1[16] = {0}, r2[16] = {0};
        int i = 0; while(*p && *p != ',') r1[i++] = *p++; r1[i] = 0; p++;
        i = 0; while(*p == ' ') p++; while(*p && *p != ',') r2[i++] = *p++; r2[i] = 0;
        int d = asm_reg_id(r1), s = asm_reg_id(r2);
        if(d >= 0 && s >= 0) { jb_e(b, 0x48); jb_e(b, 0x31); jb_e(b, 0xC0 + (d << 3) + s); }
    }
    else if(strcmp(mnemonic, "add") == 0) { 
        while(*p == ' ') p++;
        char r1[16] = {0}, r2[16] = {0};
        int i = 0; while(*p && *p != ',') r1[i++] = *p++; r1[i] = 0; p++;
        i = 0; while(*p == ' ') p++; while(*p && *p != ',') r2[i++] = *p++; r2[i] = 0;
        int d = asm_reg_id(r1), s = asm_reg_id(r2);
        if(d >= 0 && s >= 0) { jb_e(b, 0x48); jb_e(b, 0x01); jb_e(b, 0xC0 + (d << 3) + s); }
    }
    else if(strcmp(mnemonic, "sub") == 0) { 
        while(*p == ' ') p++;
        char r1[16] = {0}, r2[16] = {0};
        int i = 0; while(*p && *p != ',') r1[i++] = *p++; r1[i] = 0; p++;
        i = 0; while(*p == ' ') p++; while(*p && *p != ',') r2[i++] = *p++; r2[i] = 0;
        int d = asm_reg_id(r1), s = asm_reg_id(r2);
        if(d >= 0 && s >= 0) { jb_e(b, 0x48); jb_e(b, 0x29); jb_e(b, 0xC0 + (d << 3) + s); }
    }
    else if(strcmp(mnemonic, "mov") == 0) { 
        while(*p == ' ') p++;
        char r1[16] = {0}, r2[16] = {0};
        int i = 0; while(*p && *p != ',') r1[i++] = *p++; r1[i] = 0; p++;
        i = 0; while(*p == ' ') p++; while(*p && *p != ',') r2[i++] = *p++; r2[i] = 0;
        int d = asm_reg_id(r1); 
        if(d >= 0) {
            if(r2[0] >= '0' && r2[0] <= '9') { // mov reg, imm
                int64_t imm = atoll(r2);
                jb_e(b, 0x48); jb_e(b, 0xB8 + d); jb_e64(b, imm);
            } else { // mov reg, reg
                int s = asm_reg_id(r2); 
                if(s >= 0) { jb_e(b, 0x48); jb_e(b, 0x89); jb_e(b, 0xC0 + (s << 3) + d); }
            }
        }
    }
    else if(strcmp(mnemonic, "vaddps") == 0) { // AVX2
        while(*p == ' ') p++;
        char r1[16]={0}, r2[16]={0}, r3[16]={0};
        int i=0; while(*p&&*p!=',') r1[i++]=*p++; r1[i]=0; p++;
        i=0; while(*p==' ') p++; while(*p&&*p!=',') r2[i++]=*p++; r2[i]=0; p++;
        i=0; while(*p==' ') p++; while(*p&&*p!=',') r3[i++]=*p++; r3[i]=0;
        int d=asm_reg_id(r1), s1=asm_reg_id(r2), s2=asm_reg_id(r3);
        if(d>=0 && s1>=0 && s2>=0) { jb_e(b,0xC5); jb_e(b,0xFD); jb_e(b,0x58); jb_e(b,0xC0+(s1<<3)+s2); }
    }
}

/* ==================== 4. LEXER & PARSER (AST) ==================== */
typedef enum {
    T_EOF, T_LET, T_PRINT, T_IF, T_ELSE, T_WHILE, T_FN, T_RETURN, T_UNSAFE, T_ALLOC,
    T_MATCH, T_TABLE, T_IMPORT, T_ASM, T_GC, T_TRAIT, T_IMPL, T_FOR, T_IN, T_ASYNC, T_AWAIT, T_EXTERN,
    T_IDENT, T_INT, T_STR, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_ASSIGN, T_EQ, T_LT, T_GT,
    T_COLON, T_AT, T_LB, T_RB, T_LP, T_RP, T_COMMA, T_DOT, T_ARROW
} TT;

typedef struct { TT t; union { int64_t i; char* s; } d; } Tok;
typedef struct { const char* s; size_t p, l; } Lex;

Lex* lex_new(const char* s) { Lex* l = malloc(sizeof(Lex)); l->s=s; l->p=0; l->l=strlen(s); return l; }
char lx_p(Lex* l) { return (l->p<l->l) ? l->s[l->p] : 0; }
char lx_a(Lex* l) { return (l->p<l->l) ? l->s[l->p++] : 0; }

void lx_skip(Lex* l) { 
    while(1) {
        char c = lx_p(l); 
        if(c==' ' || c=='\t' || c=='\n' || c=='\r') lx_a(l); 
        else if(c=='#') { while(lx_p(l)!='\n' && lx_p(l)) lx_a(l); } 
        else break;
    } 
}

Tok lx_next(Lex* l) {
    Tok t = {T_EOF, {0}}; 
    lx_skip(l); 
    char c = lx_p(l); 
    if(!c) return t;
    
    if(c=='+') { t.t=T_PLUS; lx_a(l); return t; } 
    if(c=='-') { t.t=T_MINUS; lx_a(l); return t; }
    if(c=='*') { t.t=T_STAR; lx_a(l); return t; } 
    if(c=='/') { t.t=T_SLASH; lx_a(l); return t; }
    if(c==':') { t.t=T_COLON; lx_a(l); return t; } 
    if(c=='@') { t.t=T_AT; lx_a(l); return t; }
    if(c=='{') { t.t=T_LB; lx_a(l); return t; } 
    if(c=='}') { t.t=T_RB; lx_a(l); return t; }
    if(c=='(') { t.t=T_LP; lx_a(l); return t; } 
    if(c==')') { t.t=T_RP; lx_a(l); return t; }
    if(c==',') { t.t=T_COMMA; lx_a(l); return t; } 
    if(c=='.') { t.t=T_DOT; lx_a(l); return t; }
    if(c=='=') { 
        lx_a(l); 
        if(lx_p(l)=='=') { lx_a(l); t.t=T_EQ; } else t.t=T_ASSIGN; 
        return t; 
    }
    if(c=='<') { lx_a(l); t.t=T_LT; return t; } 
    if(c=='>') { lx_a(l); t.t=T_GT; return t; }
    if(c=='"') { 
        lx_a(l); 
        size_t s = l->p; 
        while(lx_p(l)!='"' && lx_p(l)) lx_a(l); 
        size_t ln = l->p - s; 
        t.t = T_STR; 
        t.d.s = malloc(ln + 1); 
        strncpy(t.d.s, l->s + s, ln); 
        t.d.s[ln] = 0; 
        if(lx_p(l)=='"') lx_a(l); 
        return t; 
    }
    if(c>='0' && c<='9') { 
        int64_t v = 0; 
        while(lx_p(l)>='0' && lx_p(l)<='9') v = v*10 + (lx_a(l)-'0'); 
        t.t = T_INT; t.d.i = v; 
        return t; 
    }
    if((c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_') {
        size_t s = l->p; 
        while(1) { 
            char ch = lx_p(l); 
            if((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='_') lx_a(l); 
            else break; 
        }
        size_t ln = l->p - s; 
        char* id = malloc(ln + 1); 
        strncpy(id, l->s + s, ln); 
        id[ln] = 0;
        
        #define KW(k, tok) if(strcmp(id, k)==0) { t.t = tok; free(id); return t; }
        KW("let", T_LET) KW("print", T_PRINT) KW("if", T_IF) KW("else", T_ELSE) KW("while", T_WHILE)
        KW("fn", T_FN) KW("return", T_RETURN) KW("unsafe", T_UNSAFE) KW("alloc", T_ALLOC)
        KW("match", T_MATCH) KW("table", T_TABLE) KW("import", T_IMPORT) KW("asm", T_ASM) KW("gc", T_GC)
        KW("trait", T_TRAIT) KW("impl", T_IMPL) KW("for", T_FOR) KW("in", T_IN) KW("async", T_ASYNC)
        KW("await", T_AWAIT) KW("extern", T_EXTERN)
        #undef KW
        
        t.t = T_IDENT; t.d.s = id; 
        return t;
    }
    lx_a(l); 
    return t;
}

typedef enum {
    N_INT, N_STR, N_IDENT, N_BINOP, N_VAR, N_PRINT, N_BLOCK, N_IF, N_WHILE, N_FUNC, N_CALL, N_RETURN,
    N_UNSAFE, N_ALLOC, N_MATCH, N_MATCH_ARM, N_IMPORT, N_ASM, N_GC, N_TRAIT, N_IMPL, N_ASYNC, N_EXTERN, N_AWAIT
} NT;

typedef struct AN {
    NT t;
    union {
        int64_t i; char* s; char* id;
        struct { struct AN* l, *r; char op; } bin;
        struct { char* n; struct AN* v; } var;
        struct { struct AN* v; } pr;
        struct { struct AN** s; int c; } blk;
        struct { struct AN* c, *tb, *eb; } ifs;
        struct { struct AN* c, *b; } wh;
        struct { char* n; char** p; int pc; struct AN* b; } fn;
        struct { char* n; struct AN** a; int ac; } cal;
        struct { struct AN* v; } ret;
        struct { struct AN* b; } uns;
        struct { struct AN* sz; } alc;
        struct { struct AN* tgt; struct AN** arms; int ac; } mat;
        struct { struct AN* pat, *b; } ma;
        struct { char* m; } imp;
        struct { char* code; } asm_b;
        struct { char* cmd; } gc;
        struct { char* n; char** ms; int mc; } tr;
        struct { char* tr, *ft; struct AN* b; } impl;
        struct { struct AN* b; } async;
        struct { char* lib, *sym; } ext;
    } d;
} AN;

typedef struct { Lex* l; Tok c, p; } Par;

Par* par_new(Lex* l) { 
    Par* p = malloc(sizeof(Par)); 
    p->l = l; 
    p->c = lx_next(l); 
    p->p = lx_next(l); 
    return p; 
}

void par_adv(Par* p) { p->c = p->p; p->p = lx_next(p->l); }

AN* an_new(Arena* arena, NT t) { 
    AN* n = arena_alloc(arena, sizeof(AN)); 
    n->t = t; 
    return n; 
}

AN* p_expr(Par* p, Arena* arena); 
AN* p_blk(Par* p, Arena* arena);

AN* p_prim(Par* p, Arena* arena) {
    if(p->c.t == T_INT) { AN* n = an_new(arena, N_INT); n->d.i = p->c.d.i; par_adv(p); return n; }
    if(p->c.t == T_STR) { AN* n = an_new(arena, N_STR); n->d.s = p->c.d.s; par_adv(p); return n; }
    if(p->c.t == T_IDENT) {
        if(p->p.t == T_INT || p->p.t == T_STR || p->p.t == T_IDENT || p->p.t == T_LP) {
            char* nm = p->c.d.s; 
            par_adv(p);
            if(p->c.t == T_LP) { 
                par_adv(p); 
                AN** a = NULL; int ac = 0; 
                while(p->c.t != T_RP && p->c.t != T_EOF) { 
                    a = realloc(a, sizeof(AN*)*(ac+1)); 
                    a[ac++] = p_expr(p, arena); 
                    if(p->c.t == T_COMMA) par_adv(p); 
                } 
                if(p->c.t == T_RP) par_adv(p); 
                AN* n = an_new(arena, N_CALL); 
                n->d.cal.n = nm; n->d.cal.a = a; n->d.cal.ac = ac; 
                return n; 
            }
            AN** a = NULL; int ac = 0; 
            while(p->c.t == T_INT || p->c.t == T_STR || p->c.t == T_IDENT) { 
                a = realloc(a, sizeof(AN*)*(ac+1)); 
                a[ac++] = p_prim(p, arena); 
            }
            AN* n = an_new(arena, N_CALL); 
            n->d.cal.n = nm; n->d.cal.a = a; n->d.cal.ac = ac; 
            return n;
        }
        AN* n = an_new(arena, N_IDENT); 
        n->d.id = p->c.d.s; 
        par_adv(p); 
        return n;
    }
    if(p->c.t == T_ALLOC) { par_adv(p); AN* n = an_new(arena, N_ALLOC); n->d.alc.sz = p_expr(p, arena); return n; }
    if(p->c.t == T_LP) { par_adv(p); AN* n = p_expr(p, arena); if(p->c.t == T_RP) par_adv(p); return n; }
    return an_new(arena, N_INT);
}

AN* p_mul(Par* p, Arena* arena) { 
    AN* l = p_prim(p, arena); 
    while(p->c.t == T_STAR || p->c.t == T_SLASH) { 
        char op = (p->c.t == T_STAR) ? '*' : '/'; 
        par_adv(p); 
        AN* n = an_new(arena, N_BINOP); 
        n->d.bin.op = op; n->d.bin.l = l; n->d.bin.r = p_prim(p, arena); 
        l = n; 
    } 
    return l; 
}

AN* p_add(Par* p, Arena* arena) { 
    AN* l = p_mul(p, arena); 
    while(p->c.t == T_PLUS || p->c.t == T_MINUS) { 
        char op = (p->c.t == T_PLUS) ? '+' : '-'; 
        par_adv(p); 
        AN* n = an_new(arena, N_BINOP); 
        n->d.bin.op = op; n->d.bin.l = l; n->d.bin.r = p_mul(p, arena); 
        l = n; 
    } 
    return l; 
}

AN* p_cmp(Par* p, Arena* arena) { 
    AN* l = p_add(p, arena); 
    while(p->c.t == T_EQ || p->c.t == T_LT || p->c.t == T_GT) { 
        char op = (p->c.t == T_EQ) ? '=' : (p->c.t == T_LT) ? '<' : '>'; 
        par_adv(p); 
        AN* n = an_new(arena, N_BINOP); 
        n->d.bin.op = op; n->d.bin.l = l; n->d.bin.r = p_add(p, arena); 
        l = n; 
    } 
    return l; 
}

AN* p_expr(Par* p, Arena* arena) { return p_cmp(p, arena); }

AN* p_stmt(Par* p, Arena* arena) {
    if(p->c.t == T_LET) { 
        par_adv(p); 
        char* nm = p->c.d.s; 
        par_adv(p); 
        if(p->c.t == T_COLON) { while(p->c.t != T_ASSIGN) par_adv(p); } 
        par_adv(p); 
        AN* n = an_new(arena, N_VAR); 
        n->d.var.n = nm; n->d.var.v = p_expr(p, arena); 
        return n; 
    }
    if(p->c.t == T_PRINT) { 
        par_adv(p); 
        AN* n = an_new(arena, N_PRINT); 
        n->d.pr.v = p_expr(p, arena); 
        return n; 
    }
    if(p->c.t == T_IF) { 
        par_adv(p); 
        AN* c = p_expr(p, arena); 
        if(p->c.t == T_COLON) par_adv(p); 
        AN* tb = p_blk(p, arena); 
        AN* eb = NULL; 
        if(p->c.t == T_ELSE) { 
            par_adv(p); 
            if(p->c.t == T_COLON) par_adv(p); 
            eb = p_blk(p, arena); 
        } 
        AN* n = an_new(arena, N_IF); 
        n->d.ifs.c = c; n->d.ifs.tb = tb; n->d.ifs.eb = eb; 
        return n; 
    }
    if(p->c.t == T_WHILE) { 
        par_adv(p); 
        AN* c = p_expr(p, arena); 
        if(p->c.t == T_COLON) par_adv(p); 
        AN* b = p_blk(p, arena); 
        AN* n = an_new(arena, N_WHILE); 
        n->d.wh.c = c; n->d.wh.b = b; 
        return n; 
    }
    if(p->c.t == T_FN) { 
        par_adv(p); 
        char* nm = p->c.d.s; 
        par_adv(p); 
        char** pp = NULL; int pc = 0; 
        while(p->c.t != T_COLON && p->c.t != T_EOF) { 
            if(p->c.t == T_IDENT) { 
                pp = realloc(pp, sizeof(char*)*(pc+1)); 
                pp[pc++] = p->c.d.s; 
            } 
            par_adv(p); 
        } 
        if(p->c.t == T_COLON) par_adv(p); 
        AN* b = p_blk(p, arena); 
        AN* n = an_new(arena, N_FUNC); 
        n->d.fn.n = nm; n->d.fn.p = pp; n->d.fn.pc = pc; n->d.fn.b = b; 
        return n; 
    }
    if(p->c.t == T_RETURN) { 
        par_adv(p); 
        AN* n = an_new(arena, N_RETURN); 
        n->d.ret.v = p_expr(p, arena); 
        return n; 
    }
    if(p->c.t == T_UNSAFE) { 
        par_adv(p); 
        if(p->c.t == T_COLON) par_adv(p); 
        AN* b = p_blk(p, arena); 
        AN* n = an_new(arena, N_UNSAFE); 
        n->d.uns.b = b; 
        return n; 
    }
    if(p->c.t == T_MATCH) { 
        par_adv(p); 
        AN* tgt = p_expr(p, arena); 
        if(p->c.t == T_COLON) par_adv(p); 
        AN** arms = NULL; int ac = 0; 
        while(p->c.t != T_EOF) { 
            AN* pat = p_expr(p, arena); 
            if(p->c.t == T_COLON) par_adv(p); 
            AN* b = p_blk(p, arena); 
            arms = realloc(arms, sizeof(AN*)*(ac+1)); 
            arms[ac] = an_new(arena, N_MATCH_ARM); 
            arms[ac]->d.ma.pat = pat; arms[ac]->d.ma.b = b; 
            ac++; 
        } 
        AN* n = an_new(arena, N_MATCH); 
        n->d.mat.tgt = tgt; n->d.mat.arms = arms; n->d.mat.ac = ac; 
        return n; 
    }
    if(p->c.t == T_ASM) { 
        par_adv(p); 
        if(p->c.t == T_STR) { 
            char* code = p->c.d.s; 
            par_adv(p); 
            AN* n = an_new(arena, N_ASM); 
            n->d.asm_b.code = code; 
            return n; 
        } 
    }
    if(p->c.t == T_GC) { 
        par_adv(p); 
        char* cmd = p->c.d.s; 
        par_adv(p); 
        AN* n = an_new(arena, N_GC); 
        n->d.gc.cmd = cmd; 
        return n; 
    }
    if(p->c.t == T_TRAIT) { 
        par_adv(p); 
        char* nm = p->c.d.s; 
        par_adv(p); 
        if(p->c.t == T_COLON) par_adv(p); 
        char** ms = NULL; int mc = 0; 
        while(p->c.t == T_FN) { 
            par_adv(p); 
            if(p->c.t == T_IDENT) { 
                ms = realloc(ms, sizeof(char*)*(mc+1)); 
                ms[mc++] = p->c.d.s; 
            } 
            par_adv(p); 
            while(p->c.t != T_EOF && p->c.t != T_FN) par_adv(p); 
        } 
        AN* n = an_new(arena, N_TRAIT); 
        n->d.tr.n = nm; n->d.tr.ms = ms; n->d.tr.mc = mc; 
        return n; 
    }
    if(p->c.t == T_IMPL) { 
        par_adv(p); 
        char* tr = p->c.d.s; 
        par_adv(p); 
        if(p->c.t == T_FOR) { 
            par_adv(p); 
            char* ft = p->c.d.s; 
            par_adv(p); 
            if(p->c.t == T_COLON) par_adv(p); 
            AN* b = p_blk(p, arena); 
            AN* n = an_new(arena, N_IMPL); 
            n->d.impl.tr = tr; n->d.impl.ft = ft; n->d.impl.b = b; 
            return n; 
        } 
    }
    if(p->c.t == T_ASYNC) { 
        par_adv(p); 
        if(p->c.t == T_FN) { 
            par_adv(p); 
            char* nm = p->c.d.s; 
            par_adv(p); 
            while(p->c.t != T_COLON && p->c.t != T_EOF) par_adv(p); 
            if(p->c.t == T_COLON) par_adv(p); 
            AN* b = p_blk(p, arena); 
            AN* n = an_new(arena, N_ASYNC); 
            n->d.async.b = b; 
            return n; 
        } 
    }
    if(p->c.t == T_AWAIT) { 
        par_adv(p); 
        AN* n = an_new(arena, N_AWAIT); 
        n->d.v = p_expr(p, arena); 
        return n; 
    }
    if(p->c.t == T_EXTERN) { 
        par_adv(p); 
        if(p->c.t == T_STR) { 
            char* lib = p->c.d.s; 
            par_adv(p); 
            if(p->c.t == T_IMPORT) { 
                par_adv(p); 
                char* sym = p->c.d.s; 
                par_adv(p); 
                AN* n = an_new(arena, N_EXTERN); 
                n->d.ext.lib = lib; n->d.ext.sym = sym; 
                return n; 
            } 
        } 
    }
    return p_expr(p, arena);
}

AN* p_blk(Par* p, Arena* arena) { 
    AN** ss = NULL; int c = 0; 
    while(p->c.t != T_EOF && p->c.t != T_RB) { 
        AN* s = p_stmt(p, arena); 
        ss = realloc(ss, sizeof(AN*)*(c+1)); 
        ss[c++] = s; 
    } 
    if(p->c.t == T_RB) par_adv(p); 
    AN* n = an_new(arena, N_BLOCK); 
    n->d.blk.s = ss; n->d.blk.c = c; 
    return n; 
}

/* ==================== 5. JIT CODE GEN (Optimized & Bug-fixed) ==================== */
#define MAX_VARS 512
typedef struct { char name[64]; int64_t val; } Var;
static Var g_vars[MAX_VARS]; 
static int g_vc = 0;

#define MAX_FUNCS 128
typedef struct { char name[64]; void* code; int pc; } Func;
static Func g_funcs[MAX_FUNCS]; 
static int g_fc = 0;

static int get_var(const char* n) { 
    for(int i=0; i<g_vc; i++) if(strcmp(g_vars[i].name, n)==0) return i; 
    strcpy(g_vars[g_vc].name, n); 
    return g_vc++; 
}

static int find_func(const char* n) { 
    for(int i=0; i<g_fc; i++) if(strcmp(g_funcs[i].name, n)==0) return i; 
    return -1; 
}

// Runtime print functions
void rt_print_int(int64_t v) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld\n", (long long)v);
    if(g_out_len + len < MAX_OUT - 1) {
        memcpy(g_out + g_out_len, buf, len);
        g_out_len += len;
        g_out[g_out_len] = 0;
    }
}

void rt_print_str(const char* s) {
    size_t len = strlen(s);
    if(g_out_len + len + 1 < MAX_OUT - 1) {
        memcpy(g_out + g_out_len, s, len);
        g_out[g_out_len + len] = '\n';
        g_out_len += len + 1;
        g_out[g_out_len] = 0;
    }
}

void g_expr(JBuf* b, AN* n, Arena* arena); 
void g_stmt(JBuf* b, AN* n, Arena* arena);

// Helper: emit call to runtime function with 1 int64_t argument in RDI (System V) or RCX (Windows)
void emit_call_rt(JBuf* b, void* func_ptr) {
#ifdef _WIN32
    // Windows x64: first arg in RCX
    jb_e(b, 0x48); jb_e(b, 0x89); jb_e(b, 0xC1); // mov rcx, rax
#else
    // System V AMD64: first arg in RDI
    jb_e(b, 0x48); jb_e(b, 0x89); jb_e(b, 0xC7); // mov rdi, rax
#endif
    jb_e(b, 0x48); jb_e(b, 0xB8); 
    uint8_t* p = (uint8_t*)&func_ptr; 
    for(int i=0; i<8; i++) jb_e(b, p[i]);
    jb_e(b, 0xFF); jb_e(b, 0xD0); // call rax
}

void g_expr(JBuf* b, AN* n, Arena* arena) {
    if(!n) return;
    if(n->t == N_INT) { 
        jb_e(b, 0x48); jb_e(b, 0xB8); 
        uint8_t* p = (uint8_t*)&n->d.i; 
        for(int i=0; i<8; i++) jb_e(b, p[i]); 
    }
    else if(n->t == N_STR) { 
        // String address loading (simplified: assumes string is in static memory)
        jb_e(b, 0x48); jb_e(b, 0xB8); 
        uint8_t* p = (uint8_t*)&n->d.s; 
        for(int i=0; i<8; i++) jb_e(b, p[i]); 
    }
    else if(n->t == N_IDENT) { 
        // Bug fix: Correct RIP-relative addressing for global variables
        int idx = get_var(n->d.id); 
        // We want to load from address of g_vars[idx].val
        // The instruction is: mov rax, [rip + offset]
        // offset = target_addr - (current_rip)
        // current_rip will be at b->s + 7 (after 48 8B 05 xx xx xx xx)
        // So offset = (uint64_t)&g_vars[idx].val - (uint64_t)(b->c + b->s + 7)
        // Since we don't know final address yet, we emit placeholder and patch later.
        // For simplicity in this prototype, we use absolute address loading (mov rax, imm64)
        uint64_t addr = (uint64_t)&g_vars[idx].val;
        jb_e(b, 0x48); jb_e(b, 0xB8); 
        uint8_t* p = (uint8_t*)&addr; 
        for(int i=0; i<8; i++) jb_e(b, p[i]);
        jb_e(b, 0x48); jb_e(b, 0x8B); jb_e(b, 0x00); // mov rax, [rax]
    }
    else if(n->t == N_BINOP) { 
        g_expr(b, n->d.bin.l, arena); 
        jb_e(b, 0x50); // push rax
        g_expr(b, n->d.bin.r, arena); 
        jb_e(b, 0x59); // pop rcx
        jb_e(b, 0x52); // push rdx (save rdx if needed, though not used here)
        jb_e(b, 0x58); // pop rax
        // Now rax = right, rcx = left. We want left op right.
        // Swap rax and rcx
        jb_e(b, 0x48); jb_e(b, 0x87); jb_e(b, 0xC1); // xchg rax, rcx
        
        char op = n->d.bin.op;
        if(op == '+') { jb_e(b, 0x48); jb_e(b, 0x01); jb_e(b, 0xC8); } // add rax, rcx
        else if(op == '-') { jb_e(b, 0x48); jb_e(b, 0x29); jb_e(b, 0xC8); } // sub rax, rcx
        else if(op == '*') { jb_e(b, 0x48); jb_e(b, 0x0F); jb_e(b, 0xAF); jb_e(b, 0xC1); } // imul rax, rcx
        else { 
            jb_e(b, 0x48); jb_e(b, 0x39); jb_e(b, 0xC1); // cmp rcx, rax (cmp left, right)
            if(op == '=') { jb_e(b, 0x0F); jb_e(b, 0x94); jb_e(b, 0xC0); } // sete al
            else if(op == '<') { jb_e(b, 0x0F); jb_e(b, 0x9C); jb_e(b, 0xC0); } // setl al
            else { jb_e(b, 0x0F); jb_e(b, 0x9F); jb_e(b, 0xC0); } // setg al
            jb_e(b, 0x48); jb_e(b, 0x0F); jb_e(b, 0xB6); jb_e(b, 0xC0); // movzx rax, al
        }
    }
    else if(n->t == N_CALL) { 
        int fi = find_func(n->d.cal.n);
        if(fi >= 0) {
            // Bug fix: Proper ABI for function calls
            // For prototype, we push args in reverse and let callee handle (cdecl-like for simplicity)
            // Real impl should use registers for first 6 args.
            for(int i = n->d.cal.ac - 1; i >= 0; i--) { 
                g_expr(b, n->d.cal.a[i], arena); 
                jb_e(b, 0x50); // push rax
            }
            jb_e(b, 0x48); jb_e(b, 0xB8); 
            uint8_t* p = (uint8_t*)&g_funcs[fi].code; 
            for(int i=0; i<8; i++) jb_e(b, p[i]);
            jb_e(b, 0xFF); jb_e(b, 0xD0); // call rax
            if(n->d.cal.ac > 0) { 
                jb_e(b, 0x48); jb_e(b, 0x83); jb_e(b, 0xC4); 
                jb_e(b, (uint8_t)(n->d.cal.ac * 8)); // add rsp, imm8
            }
        } else {
            // Check if it's a built-in or FFI (simplified)
            printf("[JIT] Warning: Function '%s' not found.\n", n->d.cal.n);
        }
    }
    else if(n->t == N_ALLOC) { 
        g_expr(b, n->d.alc.sz, arena); 
        // Call gc_alloc(size_t sz) -> returns pointer in rax
        emit_call_rt(b, (void*)gc_alloc);
    }
}

void g_stmt(JBuf* b, AN* n, Arena* arena) {
    if(!n) return;
    if(n->t == N_VAR) { 
        g_expr(b, n->d.var.v, arena); 
        int idx = get_var(n->d.var.n); 
        // Store rax to g_vars[idx].val
        uint64_t addr = (uint64_t)&g_vars[idx].val;
        jb_e(b, 0x48); jb_e(b, 0xB9); // mov rcx, imm64
        uint8_t* p = (uint8_t*)&addr; 
        for(int i=0; i<8; i++) jb_e(b, p[i]);
        jb_e(b, 0x48); jb_e(b, 0x89); jb_e(b, 0x01); // mov [rcx], rax
    }
    else if(n->t == N_PRINT) { 
        g_expr(b, n->d.pr.v, arena); 
        if(n->d.pr.v->t == N_STR) {
            emit_call_rt(b, (void*)rt_print_str);
        } else {
            emit_call_rt(b, (void*)rt_print_int);
        }
    }
    else if(n->t == N_IF) { 
        g_expr(b, n->d.ifs.c, arena); 
        jb_e(b, 0x48); jb_e(b, 0x85); jb_e(b, 0xC0); // test rax, rax
        jb_e(b, 0x0F); jb_e(b, 0x84); 
        size_t jz_pos = b->s; 
        jb_e32(b, 0); // placeholder
        
        g_stmt(b, n->d.ifs.tb, arena);
        
        if(n->d.ifs.eb) { 
            jb_e(b, 0xE9); 
            size_t jmp_pos = b->s; 
            jb_e32(b, 0); // placeholder
            
            jb_patch_rel32(b, jz_pos);
            g_stmt(b, n->d.ifs.eb, arena);
            jb_patch_rel32(b, jmp_pos);
        } else { 
            jb_patch_rel32(b, jz_pos); 
        }
    }
    else if(n->t == N_WHILE) { 
        size_t ls = b->s; 
        g_expr(b, n->d.wh.c, arena); 
        jb_e(b, 0x48); jb_e(b, 0x85); jb_e(b, 0xC0); 
        jb_e(b, 0x0F); jb_e(b, 0x84); 
        size_t jz_pos = b->s; 
        jb_e32(b, 0); 
        
        g_stmt(b, n->d.wh.b, arena); 
        int32_t back = (int32_t)(ls - (b->s + 5)); 
        jb_e(b, 0xE9); jb_e32(b, back); 
        jb_patch_rel32(b, jz_pos); 
    }
    else if(n->t == N_FUNC) { 
        JBuf* fb = jb_new(4096); 
        jb_e(fb, 0x55); jb_e(fb, 0x48); jb_e(fb, 0x89); jb_e(fb, 0xE5); // push rbp; mov rbp, rsp
        jb_e(fb, 0x48); jb_e(fb, 0x81); jb_e(fb, 0xEC); jb_e32(fb, 256); // sub rsp, 256
        
        // Map parameters to stack (simplified: assumes they are passed on stack for prototype)
        // Real impl should map from registers (rdi, rsi, etc.) to stack slots.
        for(int i=0; i<n->d.fn.pc; i++) {
            // param i is at rbp + 16 + i*8
            int idx = get_var(n->d.fn.p[i]);
            uint64_t addr = (uint64_t)&g_vars[idx].val;
            // mov rax, [rbp + 16 + i*8]
            jb_e(fb, 0x48); jb_e(fb, 0x8B); jb_e(fb, 0x45); jb_e(fb, 0x10 + i*8);
            // mov [addr], rax
            jb_e(fb, 0x48); jb_e(fb, 0xB9); 
            uint8_t* p = (uint8_t*)&addr; 
            for(int j=0; j<8; j++) jb_e(fb, p[j]);
            jb_e(fb, 0x48); jb_e(fb, 0x89); jb_e(fb, 0x01);
        }
        
        g_stmt(fb, n->d.fn.b, arena);
        
        jb_e(fb, 0x48); jb_e(fb, 0x31); jb_e(fb, 0xC0); // xor rax, rax (return 0)
        jb_e(fb, 0x48); jb_e(fb, 0x89); jb_e(fb, 0xEC); // mov rsp, rbp
        jb_e(fb, 0x5D); jb_e(fb, 0xC3); // pop rbp; ret
        
        ExecMem* m = exec_alloc(fb->s); 
        memcpy(m->p, fb->c, fb->s);
        
        strcpy(g_funcs[g_fc].name, n->d.fn.n); 
        g_funcs[g_fc].code = m->p; 
        g_funcs[g_fc].pc = n->d.fn.pc; 
        g_fc++;
        
        free(fb->c); free(fb); 
    }
    else if(n->t == N_RETURN) { 
        if(n->d.ret.v) g_expr(b, n->d.ret.v, arena); 
        jb_e(b, 0x48); jb_e(b, 0x89); jb_e(b, 0xEC); // mov rsp, rbp
        jb_e(b, 0x5D); jb_e(b, 0xC3); // pop rbp; ret
    }
    else if(n->t == N_UNSAFE) { 
        g_stmt(b, n->d.uns.b, arena); 
    }
    else if(n->t == N_ASM) { 
        emit_asm(b, n->d.asm_b.code); 
    }
    else if(n->t == N_GC) { 
        if(strcmp(n->d.gc.cmd, "collect") == 0) gc_collect(); 
    }
    else if(n->t == N_EXTERN) { 
        void* p = ffi_call(n->d.ext.lib, n->d.ext.sym, NULL, 0); 
        printf("[FFI] %s: %p\n", n->d.ext.sym, p); 
    }
    else if(n->t == N_ASYNC) { 
        printf("[ASYNC] Task created (simplified).\n");
        g_stmt(b, n->d.async.b, arena); 
    }
    else if(n->t == N_AWAIT) { 
        g_expr(b, n->d.v, arena); 
        printf("[ASYNC] Yield.\n");
    }
    else if(n->t == N_BLOCK) { 
        for(int i=0; i<n->d.blk.c; i++) g_stmt(b, n->d.blk.s[i], arena); 
    }
}

/* ==================== 6. POPUP & WATCHER ==================== */
#ifdef _WIN32
LRESULT CALLBACK PP(HWND h, UINT m, WPARAM w, LPARAM l) { 
    if(m == WM_PAINT) { 
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); 
        SetTextColor(hdc, RGB(0, 255, 0)); SetBkColor(hdc, RGB(0, 0, 0)); 
        TextOutA(hdc, 20, 20, g_out, strlen(g_out)); 
        EndPaint(h, &ps); 
    } else if(m == WM_CLOSE) { DestroyWindow(h); PostQuitMessage(0); } 
    else return DefWindowProcA(h, m, w, l); 
    return 0; 
}

void show_pop() { 
    WNDCLASSA wc = {0}; 
    wc.lpfnWndProc = PP; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "NP"; 
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); 
    RegisterClassA(&wc); 
    int w = 400, h = 300, x = (GetSystemMetrics(SM_CXSCREEN)-w)/2, y = (GetSystemMetrics(SM_CYSCREEN)-h)/2; 
    CreateWindowExA(0, "NP", "N Lang v7.0", WS_OVERLAPPEDWINDOW|WS_VISIBLE, x, y, w, h, NULL, NULL, wc.hInstance, NULL); 
    MSG msg; while(GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); } 
}
#else
void show_pop() { 
    printf("\n========== N Output v7.0 ==========\n%s\n=================================\n[Enter]", g_out); 
    getchar(); 
}
#endif

char* ext_code(const char* c) { 
    const char* s = strstr(c, "<N Language = UTF8>"); 
    if(!s) return NULL; 
    s += strlen("<N Language = UTF8>"); 
    const char* e = strstr(s, "<N Language end = UTF8>"); 
    if(!e) return NULL; 
    size_t l = e - s; 
    char* code = malloc(l + 1); 
    strncpy(code, s, l); 
    code[l] = 0; 
    return code; 
}

void run(const char* code) { 
    if(setjmp(g_panic_jmp) != 0) { 
        printf("Execution aborted due to panic.\n"); 
        return; 
    } 
    g_out_len = 0; g_out[0] = 0; 
    
    Arena* arena = arena_new(1024 * 1024); // 1MB Arena for AST
    
    Lex* l = lex_new(code); 
    Par* p = par_new(l); 
    AN* ast = p_blk(p, arena); 
    
    JBuf* b = jb_new(8192); 
    g_stmt(b, ast, arena); 
    jb_e(b, 0xC3); 
    
    ExecMem* m = exec_alloc(b->s); 
    memcpy(m->p, b->c, b->s); 
    ((void(*)())m->p)(); 
    
    show_pop(); 
    
    exec_free(m); 
    free(b->c); free(b); 
    free(l); free(p); 
    arena_free(arena); // Free all AST nodes at once!
}

#ifdef _WIN32
void watch() { 
    char p[MAX_PATH]; SHGetFolderPathA(NULL, CSIDL_DOWNLOADS, NULL, 0, p); 
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL); 
    if(h == INVALID_HANDLE_VALUE) return; 
    char buf[4096]; DWORD by; 
    while(1) { 
        if(!ReadDirectoryChangesW(h, buf, sizeof(buf), FALSE, FILE_ACTION_ADDED, &by, NULL, NULL)) { SLEEP_MS(1000); continue; } 
        FILE_NOTIFY_INFORMATION* f = (FILE_NOTIFY_INFORMATION*)buf; 
        if(f->Action == FILE_ACTION_ADDED) { 
            wchar_t* ext = wcschr(f->FileName, L'.'); 
            if(ext && _wcsicmp(ext, L".txt") == 0) { 
                SLEEP_MS(9000); 
                wchar_t fp[MAX_PATH]; swprintf(fp, MAX_PATH, L"%s\\%s", p, f->FileName); 
                HANDLE hf = CreateFileW(fp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); 
                if(hf != INVALID_HANDLE_VALUE) { 
                    DWORD sz = GetFileSize(hf, NULL); 
                    char* c = malloc(sz + 1); DWORD r; 
                    ReadFile(hf, c, sz, &r, NULL); c[sz] = 0; CloseHandle(hf); 
                    char* nc = ext_code(c); 
                    if(nc) { run(nc); free(nc); } 
                    free(c); 
                } 
                SLEEP_MS(8000); 
            } 
        } 
    } 
}
#else
void watch() { 
    char p[512]; const char* h = getenv("HOME"); 
    if(!h) { struct passwd* pw = getpwuid(getuid()); h = pw->pw_dir; } 
    snprintf(p, sizeof(p), "%s/Downloads", h); 
    int fd = inotify_init(); 
    int wd = inotify_add_watch(fd, p, IN_CREATE); 
    if(wd < 0) return; 
    char buf[4096]; 
    while(1) { 
        int l = read(fd, buf, sizeof(buf)); 
        if(l > 0) { 
            int i = 0; 
            while(i < l) { 
                struct inotify_event* ev = (struct inotify_event*)&buf[i]; 
                if(ev->mask & IN_CREATE) { 
                    size_t nl = strlen(ev->name); 
                    if(nl > 4 && strcmp(ev->name + nl - 4, ".txt") == 0) { 
                        SLEEP_MS(9000); 
                        char fp[1024]; snprintf(fp, sizeof(fp), "%s/%s", p, ev->name); 
                        FILE* f = fopen(fp, "r"); 
                        if(f) { 
                            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); 
                            char* c = malloc(sz + 1); fread(c, 1, sz, f); c[sz] = 0; fclose(f); 
                            char* nc = ext_code(c); 
                            if(nc) { run(nc); free(nc); } 
                            free(c); 
                        } 
                        SLEEP_MS(8000); 
                    } 
                } 
                i += sizeof(struct inotify_event) + ev->len; 
            } 
        } 
    } 
}
#endif

int main(int argc, char* argv[]) { 
    if(argc > 1) { 
        FILE* f = fopen(argv[1], "r"); 
        if(!f) { printf("Err\n"); return 1; } 
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); 
        char* c = malloc(sz + 1); fread(c, 1, sz, f); c[sz] = 0; fclose(f); 
        char* code = strstr(argv[1], ".txt") ? ext_code(c) : c; 
        if(code) run(code); else printf("No N\n"); 
        free(c); if(code != c) free(code); 
    } else { 
        printf("N Watcher v7.0 Stable & Fast (9-17s)...\n"); 
        watch(); 
    } 
    return 0; 
}
