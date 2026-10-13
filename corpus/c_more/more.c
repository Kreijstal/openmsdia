/* OpenMSDIA test corpus: more C types and constructs. Freestanding. */
#include <stdbool.h>
#include <stddef.h>

#pragma pack(push, 1)
struct packed {
    char c;
    int i;
    short s;
};
#pragma pack(pop)

struct with_anon {
    int kind;
    union {
        int as_int;
        float as_float;
    };
    struct {
        short lo, hi;
    } pair;
};

typedef wchar_t wide;
typedef unsigned short ushort_t;
typedef long long_t;
typedef unsigned long ulong_t;
typedef long double ldouble;

struct many {
    bool flag;
    wchar_t w;
    long l;
    unsigned long ul;
    long double ld;
    __int128 big;
    unsigned __int128 ubig;
    float f[2][3];
    char *const cp;
    const char *volatile vcp;
    int *restrict rp;
    void *vp;
    void (**fpp)(void);
    _Bool b2;
    signed char sc;
    unsigned char uc;
};

enum small_enum : unsigned char { SE_A, SE_B = 200 };
enum big_enum { BE_A = 0x7fffffff, BE_B = -0x7fffffff - 1 };

struct many g_many;
struct packed g_packed = {1, 2, 3};
struct with_anon g_anon;
enum small_enum g_small = SE_B;
enum big_enum g_big = BE_B;
static const int table[5] = {1, 2, 3, 5, 8};
_Thread_local int tls_counter;
unsigned int _tls_index; /* normally from the CRT */

static int labels(int n)
{
    int acc = 0;
again:
    acc += n;
    if (--n > 0)
        goto again;
    return acc;
}

int recurse(int depth)
{
    if (depth <= 0)
        return 0;
    {
        int inner = depth * 2;
        {
            int innermost = inner + table[depth % 5];
            return innermost + recurse(depth - 1);
        }
    }
}

int start(void)
{
    g_many.l = labels(3);
    g_many.flag = true;
    g_anon.as_int = recurse(4);
    tls_counter++;
    return (int)g_many.l + g_anon.as_int + g_packed.i + (int)g_small + (g_big < 0);
}
