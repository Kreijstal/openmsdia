// OpenMSDIA test corpus: more C++ constructs. Adjustor thunks (an override
// of a second base's virtual function), variadic functions, goto labels,
// bit fields and static members of a class, nested types, pointers to
// member functions, lambdas, cv-qualified methods. Freestanding.
#include <stdarg.h>

#ifdef _MSC_VER
extern "C" int _fltused = 0;
extern "C" int __cdecl _purecall() { for (;;) {} }
#endif
extern "C" void __cxa_pure_virtual() { for (;;) {} }
void operator delete(void *) noexcept {}
void operator delete(void *, decltype(sizeof 0)) noexcept {}

struct Left {
    int l = 1;
    virtual int left() { return l; }
    virtual ~Left() {}
};

struct Right {
    int r = 2;
    virtual int right() { return r; }
    virtual int both() { return r * 2; }
    virtual ~Right() {}
};

struct Both : Left, Right {
    int b = 3;
    int right() override { return b + r; }
    int both() override { return b * 2; }
};

namespace ns {
struct Counter {
    enum class Step : short { One = 1, Two = 2 };
    unsigned value : 12;
    unsigned flag : 1;
    signed delta : 3;
    static int instances;
    Counter() : value(0), flag(0), delta(0) { ++instances; }
    int bump(Step s) const volatile { return (int)value + (int)s; }
    int add(int x)
    {
        value += x;
        return (int)value;
    }
    static int count() { return instances; }
    struct Nested {
        long long id;
        int get() const { return (int)id; }
    };
};
int Counter::instances = 0;
}  // namespace ns

static int sum_ints(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static int with_labels(int n)
{
    int acc = 0;
top:
    if (n <= 0)
        goto done;
    acc += n--;
    goto top;
done:
    return acc;
}

typedef int (ns::Counter::*AddFn)(int);
AddFn g_add = &ns::Counter::add;
int (*g_variadic)(int, ...) = sum_ints;

extern "C" int start()
{
    Both b;
    Right *rp = &b;
    Left *lp = &b;
    ns::Counter c;
    int v = (c.*g_add)(5);
    auto lam = [&v](int x) { return v + x; };
    ns::Counter::Nested nested{7};
    int r = rp->right() + rp->both() + lp->left() + g_variadic(3, 1, 2, 3) + with_labels(4);
    r += lam(2) + nested.get() + c.bump(ns::Counter::Step::Two) + ns::Counter::count();
    return r;
}
