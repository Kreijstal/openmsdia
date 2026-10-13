/* OpenMSDIA test corpus: what the 32-bit x86 builds need from a runtime. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
#ifdef _MSC_VER
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
#pragma function(memcpy, memset)
#endif

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
#ifdef __cplusplus
}
#endif
