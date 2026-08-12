#ifndef _ECLIPSE_STRING_H
#define _ECLIPSE_STRING_H

unsigned int strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned int n);

void *memcpy(void *dst, const void *src, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int val, unsigned int n);

#endif
