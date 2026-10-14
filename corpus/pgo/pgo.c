/* OpenMSDIA corpus: a program built with MSVC profile-guided optimization
 * (corpus/pgo/build_pgo.ps1, CI only). The training run never takes the
 * error paths, so the optimizer separates them from the hot code
 * (S_SEPCODE) and records call counts (S_CALLERS, S_CALLEES). Links the C
 * runtime, unlike the rest of the corpus, because the instrumented build
 * needs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
    int key;
    int value;
};

static __declspec(noinline) int checksum(const struct item *items, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].key < 0) {
            /* Never taken in training: cold. */
            fprintf(stderr, "bad key %d at %d\n", items[i].key, i);
            fflush(stderr);
            abort();
        }
        sum += items[i].key * 31 + items[i].value;
    }
    return sum;
}

static __declspec(noinline) int lookup(const struct item *items, int n, int key)
{
    for (int i = 0; i < n; i++)
        if (items[i].key == key) return items[i].value;
    if (key == -12345) {
        /* Never taken in training: cold. */
        char buf[64];
        snprintf(buf, sizeof buf, "missing %d", key);
        puts(buf);
        exit(3);
    }
    return -1;
}

__declspec(noinline) int hot_path(int x)
{
    return x * 2 + 1;
}

__declspec(noinline) int cold_path(int x)
{
    char buf[128];
    snprintf(buf, sizeof buf, "cold %d", x);
    return (int)strlen(buf);
}

__declspec(noinline) int dispatch(int x)
{
    if (x == 7777777) return cold_path(x);
    return hot_path(x);
}

int main(int argc, char **argv)
{
    struct item items[64];
    for (int i = 0; i < 64; i++) {
        items[i].key = i * 3;
        items[i].value = i ^ 0x55;
    }
    int total = 0;
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    for (int r = 0; r < rounds; r++) {
        total += checksum(items, 64);
        total += lookup(items, 64, (r * 7) % 200);
        total += dispatch(r);
    }
    printf("%d\n", total & 0xffff);
    return 0;
}
