#include "cmdline.h"

#include <stdbool.h>

#include "string.h"
#include "mem/alloc.h"
#include "uacpi/platform/compiler.h"

char *strstr(const char *haystack, const char *needle) {
    size_t nl=strlen(needle);
    size_t hl=strlen(haystack);
    size_t i;
    if (!nl) goto found;
    if (nl>hl) return 0;
    for (i=hl-nl+1; uacpi_likely(i); --i) {
        if (*haystack==*needle && !memcmp(haystack,needle,nl))
            found:
                  return (char*)haystack;
        ++haystack;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    register unsigned int i;
    for (; *s; s++)
        for (i=0; accept[i]; i++)
            if (*s == accept[i])
                return (char*)s;
    return 0;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    register const unsigned char* a=(const unsigned char*)s1;
    register const unsigned char* b=(const unsigned char*)s2;
    register const unsigned char* fini=a+n;
    while (a!=fini) {
        register int res=*a-*b;
        if (res) return res;
        if (!*a) return 0;
        ++a; ++b;
    }
    return 0;
}

void *memccpy(void *dst, const void *src, int c, size_t count) {
    char *a = dst;
    const char *b = src;
    while (count--)
    {
        *a++ = *b;
        if (*b==c)
        {
            return (void *)a;
        }
        b++;
    }
    return 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
    memset(dest,0,n);
    memccpy(dest,src,0,n);
    return dest;
}

char* cmdline_get_option_value(const char* cmdline, const char* key) {
    size_t key_len = strlen(key);
    const char* p = cmdline;

    while ((p = strstr(p, "--")) != NULL) {
        if (strncmp(p + 2, key, key_len) == 0 && p[key_len + 2] == '=') {
            const char* val_start = p + key_len + 3;
            const char* val_end = strpbrk(val_start, " \t\n\r");

            size_t len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            char* value = mem_alloc(len + 1);
            if (!value) return NULL;

            strncpy(value, val_start, len);
            value[len] = '\0';
            return value;
        }
        p += 2; // skip past "--"
    }

    return NULL;
}

/**
 * Check if a boolean flag (--key) exists in the command line string.
 */
bool cmdline_has_flag(const char* cmdline, const char* key) {
    size_t key_len = strlen(key);
    const char* p = cmdline;

    while ((p = strstr(p, "--")) != NULL) {
        if (strncmp(p + 2, key, key_len) == 0) {
            char end = p[key_len + 2];
            if (end == '\0' || end == ' ' || end == '\t' || end == '\n' || end == '\r')
                return true;
        }
        p += 2;
    }

    return false;
}
