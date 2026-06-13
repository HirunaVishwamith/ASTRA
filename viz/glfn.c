/* glfn.c — define + load the GL entry points listed in glfn_list.h. */
#include "glfn.h"
#include <stdio.h>

/* define the pointers (zero-initialised) */
#define GLFN(type, name) type pgl_##name = NULL;
#include "glfn_list.h"
#undef GLFN

int glfn_load(void *(*getproc)(const char *)) {
    int missing = 0;
#define GLFN(type, name)                                            \
    pgl_##name = (type)getproc(#name);                              \
    if (!pgl_##name) { fprintf(stderr, "glfn: missing %s\n", #name); missing++; }
#include "glfn_list.h"
#undef GLFN
    return missing == 0;
}
