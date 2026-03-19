#include <stdlib.h>
#include <string.h>
#include "sds.h"

sds sdsempty(void) {
    struct sdshdr *sh = malloc(sizeof(struct sdshdr)+1);
    sh->len = 0; sh->free = 0; sh->buf[0] = '\0';
    return (char*)sh->buf;
}

sds sdscatlen(sds s, const void *t, size_t len) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    sh = realloc(sh, sizeof(struct sdshdr)+sh->len+len+1);
    memcpy(sh->buf+sh->len, t, len);
    sh->len += len; sh->buf[sh->len] = '\0';
    return (char*)sh->buf;
}

void sdsfree(sds s) {
    if (s == NULL) return;
    free(s-sizeof(struct sdshdr));
}

void sdsclear(sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    sh->len = 0; sh->buf[0] = '\0';
}
