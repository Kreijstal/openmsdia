// OpenMSDIA test corpus (MSVC only): constructs first seen in the C
// runtime's debug information. Member typedefs of classes, enums, base
// types and pointers next to a true nested class; a lambda's function
// pointer typedefs; a struct that is only declared; const
// std::nullptr_t; 16-bit constants of 0x8000 and more; heap allocation
// sites (__declspec(allocator)); noexcept functions; methods of a class
// in an anonymous namespace and of a local class. Built with /guard:cf
// so functions carry the control-flow-guard flag. Freestanding.

extern "C" int _fltused = 0;
// /guard:cf calls through these (the C runtime defines them); never run.
extern "C" void *__guard_dispatch_icall_fptr = nullptr;
extern "C" void *__guard_check_icall_fptr = nullptr;
void operator delete(void *) noexcept {}
void operator delete(void *, decltype(sizeof 0)) noexcept {}

typedef decltype(sizeof 0) size_t;

struct Plain {
    int x;
};

enum Colour { Red, Green };

struct Traits {
    typedef Plain plain_type;     // a class not nested here: a typedef
    typedef Colour colour_type;   // an enum: a typedef
    typedef char char_type;       // a base type
    typedef void *handle_type;    // a simple pointer type
    typedef int *int_pointer;     // a pointer record
    struct Inner {                // a true nested class
        int y;
        int twice() const { return y * 2; }
    };
    typedef Inner inner_type;     // a nested class under another name
    char_type c;
    handle_type h;
    static int value(plain_type p) { return p.x; }
};

struct Opaque;                    // declared, never defined
Opaque *g_opaque = nullptr;

const decltype(nullptr) g_null = nullptr;

enum class Wide : unsigned short { Low = 1, High = 0x8000, Top = 0xFFFF };
static const unsigned short kBig = 0x9000;

static char g_heap[256];
static size_t g_used;

__declspec(allocator) __declspec(noinline) void *heap_alloc(size_t n)
{
    void *p = g_heap + g_used;
    g_used += (n + 7) & ~(size_t)7;
    return p;
}

struct Node {
    int value;
    Node *next;
};

__declspec(noinline) Node *make_node(int v) noexcept
{
    Node *n = (Node *)heap_alloc(sizeof(Node));
    n->value = v;
    n->next = nullptr;
    return n;
}

__declspec(noinline) int no_throw(int a) noexcept { return a + 1; }

namespace {
struct Hidden {
    int v;
    __declspec(noinline) int get() const { return v; }
};
}  // namespace

__declspec(noinline) int use_local(int a)
{
    struct Local {
        int w;
        __declspec(noinline) int scaled(int k) const { return w * k; }
    };
    Local l{a};
    return l.scaled(3);
}

typedef int (*callback)(int);

__declspec(noinline) int call(callback f, int a) { return f(a); }

extern "C" int start()
{
    Traits t{};
    t.c = 'a';
    t.h = &t;
    Traits::Inner in{4};
    Plain p{5};
    Hidden h{6};
    Node *n = make_node(7);
    auto lam = [](int x) { return x * 3; };
    int r = Traits::value(p) + in.twice() + h.get() + n->value + no_throw(1) + use_local(2) + call(lam, 3);
    r += (int)Wide::High + kBig + (g_opaque ? 1 : 0) + (g_null == nullptr ? 1 : 0);
    return r;
}
