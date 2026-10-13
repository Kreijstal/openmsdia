#include "shapes.h"

static u32 util_calls;

int area(const struct rect *r)
{
    int w = r->max.x - r->min.x;
    int h = r->max.y - r->min.y;
    util_calls++;
    return w * h;
}

int apply(binop f, int a, int b)
{
    util_calls++;
    return f(a, b);
}

u64 sum_list(const node_t *n)
{
    u64 s = 0;
    while (n) {
        s += (u64)n->counter;
        n = n->next;
    }
    {
        i8 tiny = (i8)s;
        s += (u64)tiny;
    }
    return s;
}
