# N Programming Language Official Guide (v7.0)

## 1. 언어 소개 및 철학

N 언어는 "One Language, All Levels"를 철학으로 하는 하이브리드 프로그래밍 언어입니다. 스크립팅의 간편함과 시스템 프로그래밍의 저수준 제어력을 하나의 문법으로 통합합니다.

핵심 철학:
- Space is the dot: 객체 접근, 함수 호출, 모듈 참조를 공백으로 구분합니다.
- 점진적 타이핑: 타입을 적으면 정적, 안 적으면 동적으로 동작합니다.
- 문서 즉 코드: .txt 파일 안에 코드를 숨기고 다운로드 시 자동 실행할 수 있습니다.
- 단일 파일, Zero Dependency: 컴파일러 자체가 외부 라이브러리 없이 단일 C 파일로 구성됩니다.

---

## 2. 시작하기 (컴파일 및 행)

N 언어의 JIT 컴파일러는 단일 C 파일(n-jit.c)로 제공됩니다.

### 컴파일 방법

Linux/macOS 환경:
gcc -o n-jit n-jit.c -ldl -lpthread

Windows 환경 (MSVC):
cl n-jit.c /Fe:n-jit.exe

### 실행 방법

일반 N 파일 실:
./n-jit program.n

REPL(대화형) 모드 실행:
./n-jit --repl

백그라운드 다운로드 감시 모드 실행:
./n-jit

---

## 3. 기본 문법

### 변수 선언과 할당

let 키워드를 사용하여 변수를 선언합니다. 타입을 명시하지 않으면 동적 타입으로 추론됩니다.

let x = 10
let name = "N Language"
let is_active = true

# 변수 재할당
x = 20

### 연산자

연산자의 양옆에는 반드시 공백이 있어야 합니다.

let a = 10 + 20
let b = a * 2
let c = b / 5
let d = a - b

# 비교 연산자
let is_equal = (a == 20)
let is_greater = (a > 10)

### 문자열

큰따옴표 또는 작은따옴표를 사용합니다.

let greeting = "Hello, World!"
let multiline = "
    This is a
    multiline string.
"

---

## 4. 제어 흐름

### 조건문 (if / else)

let score = 85

if score >= 90:
    print "Excellent"
else:
    if score >= 80:
        print "Good"
    else:
        print "Keep trying"

### 반복문 (while)

let count = 0

while count < 5:
    print count
    count = count + 1

### 패턴 매칭 (match)

Rust 스타일의 강력한 패턴 매칭을 지원합니다.

let status_code = 404

match status_code:
    200:
        print "OK"
    404:
        print "Not Found"
    500:
        print "Server Error"
    _:
        print "Unknown Status"

---

## 5. 함수

### 함수 정의와 호출

fn 키워드를 사용합니다. 반환 타입은 선택 사항입니다.

fn add a b:
    return a + b

fn greet name:
    print "Hello, " + name

# 호출 (공백으로 인자 구분)
let result = add 10 20
greet "Wind"

### 재귀 함수

fn factorial n:
    if n <= 1:
        return 1
    return n * factorial (n - 1)

let fact_5 = factorial 5
print fact_5

---

## 6. 테이블과 데이터 구조

### Lua 스타일 테이블

키-값 쌍을 중괄호로 감싸 정의합니다.

let user = {
    "name" "Alice"
    "age" 30
    "role" "Admin"
}

# 값 가져오기
let name = user { "name" }
print name

---

## 7. 저수준 프로그래밍 (Unsafe & ASM)

N 언어는 VM 기반이지만, unsafe 블록을 통해 직접 메모리 제어와 인라인 어셈블리를 사용할 수 있습니다.

### 메모리 할당 및 포인터

unsafe:
    # 8바이트 메모리 할당
    let ptr = alloc 8
    
    # 포인터를 통한 값 저장 및 읽기
    ptr @ 0 = 42
    let val = ptr @ 0
    
    print val
    
    # 메모리 해제
    dealloc ptr

### 인라인 어셈블리 (x86_64 / AVX2)

asm 키워드를 사용하여 직접 머신 코드를 삽입할 수 있습니다.

unsafe:
    asm "mov rax, 100"
    asm "add rax, 50"
    asm "ret"

---

## 8. 고급 기능

### Traits (트레이트)

코드 재사용과 추상화를 위한 트레이트 시스템을 제공합니다.

trait Drawable:
    fn draw self

class Circle:
    let radius: float

impl Drawable for Circle:
    fn draw self:
        print "Drawing circle with radius " + self radius

### Async / Await

비동기 작업을 위한 코루틴 기반 런타임을 지원합니다.

async fn fetch_data url:
    let response = await http_get url
    return response body

async fn main:
    let task1 = fetch_data "api1.com"
    let data = await task1
    print data

### FFI (외부 함수 인터페이스)

동적 라이브러리를 로드하고 함수를 호출할 수 있습니다.

extern "libc.so" import "printf"

### GC (가비지 컬렉션) 수동 제어

gc collect
gc disable

---

## 9. .txt 파일 자동 행 시스템

N 언어의 가장 독특한 기능입니다. 일반 텍스트 파일 안에 N 코드를 숨겨두고, 다운로드되는 순간 자동으로 실행할 수 있습니다.

### 태그 문법

코드 시작과 끝에 다음 태그를 사용합니다.

<N Language = UTF8>
print "This code runs automatically!"
</N Language end = UTF8>

### 작동 원리

1. 사용자가 .txt 파일을 다운로드합니다.
2. N Watcher Daemon이 다운로드 폴더를 감시합니다.
3. 파일 생성 후 9초에서 17초 사이의 시간 윈도우 동안 파일을 스캔합니다.
4. 태그가 발견되면 JIT 컴파일 후 화면 정중앙에 팝업으로 결과를 표시합니다.
5. 팝업은 사용자가 X 버튼을 눌러야만 닫힙니다.

### 예제: 숨겨진 스크립트가 포함된 메모장 파일

프로젝트 회의록
1. N 언어 v7.0 출시 준비
2. 마케팅 전략 수립

<N Language = UTF8>
import Core
let status = "Ready"
print "System Status: " + status
</N Language end = UTF8>

다음 회의 일정: 금요일 오후 3시

---

## 10. 전체 종합 예제

아래는 N 언어의 주요 기능을 모두 포함한 종합 예제입니다.

# N Language Comprehensive Example

fn fibonacci n:
    if n <= 1:
        return n
    return fibonacci (n - 1) + fibonacci (n - 2)

fn main:
    print "Starting N Language Demo..."
    
    let limit = 10
    let i = 0
    
    while i < limit:
        let fib = fibonacci i
        print fib
        i = i + 1
    
    unsafe:
        let buffer = alloc 64
        buffer @ 0 = 99
        let val = buffer @ 0
        print "Unsafe memory value: " + val
        dealloc buffer
    
    print "Demo finished."

main

---

## 부록: 예약어 목록

let, fn, if, else, while, for, in, return, print
match, table, unsafe, alloc, dealloc, asm
trait, impl, async, await, extern, gc
true, false, import, from

이 가이드는 N 언어 v7.0 "Stable & Fast" 버전을 기준으로 작성되었습니다.
