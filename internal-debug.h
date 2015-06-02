#ifndef __INTERNAL_DEBUG_H__
#define __INTERNAL_DEBUG_H__
#include <stdio.h>
#include <unistd.h>

#define DD(...) fprintf(stdout, "*****:"__VA_ARGS__)

#endif
