#include "shapes.hpp"

// Freestanding runtime support for the corpus.
extern "C" void __cxa_pure_virtual() { for (;;) {} }
extern "C" void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
extern "C" void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--)
        *p++ = *q++;
    return d;
}
void operator delete(void *) noexcept {}
void operator delete(void *, unsigned long long) noexcept {}

typedef long long (*Callback)(int);
using Matrix = int[2][2];

struct Outer {
    struct Inner {
        int value;
        static const int limit = 10;
    };
    Inner inner;
    union {
        int as_int;
        float as_float;
    };
    enum Mode { Off, On } mode;
};

int &ref_to_global();
int global_value = 5;
int &ref_to_global() { return global_value; }

Outer outer_global;
Matrix identity = {{1, 0}, {0, 1}};
geo::Diamond diamond;
static int (*fptr)(int) = nullptr;
int Outer_member_ptr_target = 0;
int Outer::Inner::*member_ptr = &Outer::Inner::value;

static long long square(int x) { return (long long)x * x; }

extern "C" int start()
{
    geo::Circle c(2.0);
    geo::Square s(3.0);
    geo::LabelledCircle lc(1.0, "lc");
    geo::Shape *shapes[3] = {&c, &s, &lc};
    double total = geo::total_area(shapes, 3);
    lc.rename("renamed");
    Callback cb = square;
    long long sq = cb(7);
    Outer o{};
    o.inner.*member_ptr = Outer::Inner::limit;
    o.mode = Outer::On;
    o.as_int = 3;
    int &r = ref_to_global();
    r += o.inner.value + diamond.vb + diamond.d + identity[1][1];
    outer_global = o;
    (void)fptr;
    return (int)total + (int)sq + geo::use_templates() + r + c.id();
}
