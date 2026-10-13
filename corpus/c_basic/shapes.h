/* OpenMSDIA test corpus: C types. Freestanding, no CRT. */
#ifndef CORPUS_SHAPES_H
#define CORPUS_SHAPES_H

typedef unsigned int u32;
typedef signed char i8;
typedef unsigned long long u64;

enum color { RED, GREEN = 5, BLUE, NEGATIVE = -3 };
typedef enum { SMALL = 1, LARGE = 1000000 } size_class;

struct point {
    int x, y;
};

struct rect {
    struct point min, max;
    enum color fill;
};

union value {
    int i;
    float f;
    double d;
    char bytes[8];
};

struct flags {
    unsigned a : 1;
    unsigned b : 3;
    signed c : 4;
    unsigned long long wide : 40;
};

struct node;
typedef struct node node_t;
struct node {
    node_t *next;
    const char *label;
    volatile int counter;
    const volatile u32 *cvp;
    int matrix[3][4];
    void (*callback)(struct node *, int);
};

typedef int (*binop)(int, int);

extern int global_counter;
extern const struct rect unit_rect;
int area(const struct rect *r);
int apply(binop f, int a, int b);
u64 sum_list(const node_t *n);

#endif
