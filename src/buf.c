#include "internal.h"

#include <stdlib.h>
#include <string.h>

int huv_buf_append(char **buf, size_t *len, size_t *cap, const char *data,
                    size_t n, size_t hard_cap)
{
    if (*len + n + 1 > hard_cap)
        return -1;
    if (*len + n + 1 > *cap) {
        size_t newcap = *cap ? *cap * 2 : 256;
        while (newcap < *len + n + 1) {
            size_t next = newcap * 2;
            if (next <= newcap || next > hard_cap) {
                newcap = hard_cap;
                break;
            }
            newcap = next;
        }
        char *nb = realloc(*buf, newcap);
        if (!nb)
            return -1;
        *buf = nb;
        *cap = newcap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

int huv_buf_append_nul(char **buf, size_t *len, size_t *cap, size_t hard_cap)
{
    char nul = '\0';
    return huv_buf_append(buf, len, cap, &nul, 1, hard_cap);
}
