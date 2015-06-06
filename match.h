#ifndef MATCH_H
#define MATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int	match(char*, char*);
int	matchhere(char*, char*);
int	matchstar(int, char*, char*);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

