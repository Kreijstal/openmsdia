#include "shapes.h"

#ifdef _MSC_VER
int _fltused; /* normally from the CRT */
#endif

int global_counter = 42;
static int file_static = 7;
const struct rect unit_rect = {{0, 0}, {1, 1}, GREEN};
char message[] = "hello";
double ratios[4] = {0.5, 1.5, 2.5, 3.5};
union value shared_value;
struct flags global_flags = {1, 5, -2, 123456789ULL};
static node_t nodes[3];

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }

static void bump(struct node *n, int by)
{
    n->counter += by;
}

static int count_up(int limit)
{
    static int calls;
    int total = 0;
    calls++;
    for (int i = 0; i < limit; i++) {
        int sq = i * i;
        total += sq;
    }
    return total + calls;
}

int __stdcall stdcall_fn(int a, short b, char c) { return a + b + c; }

int start(void)
{
    struct rect r = {{1, 2}, {4, 6}, BLUE};
    size_class sc = LARGE;
    enum color c = RED;
    int result = area(&r);
    binop ops[2] = {add, mul};

    for (int i = 0; i < 3; i++) {
        nodes[i].next = i < 2 ? &nodes[i + 1] : 0;
        nodes[i].label = message;
        nodes[i].callback = bump;
        nodes[i].callback(&nodes[i], i);
    }
    result += apply(ops[0], 3, 4) + apply(ops[1], 5, 6);
    result += (int)sum_list(&nodes[0]);
    result += count_up(5) + file_static + (int)sc + (int)c;
    shared_value.d = ratios[1];
    global_counter = result + stdcall_fn(1, 2, 3) + global_flags.c;
    return global_counter;
}
