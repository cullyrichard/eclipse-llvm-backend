#ifndef _ECLIPSE_STRING_H
#define _ECLIPSE_STRING_H

/* No stddef.h on this target -- see stdlib.h's identical NULL comment. */
#ifndef NULL
#define NULL ((void *)0)
#endif

unsigned int strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned int n);
char *strncpy(char *dst, const char *src, unsigned int n);
char *strncat(char *dst, const char *src, unsigned int n);
char *strdup(const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

void *memcpy(void *dst, const void *src, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int val, unsigned int n);
int memcmp(const void *a, const void *b, unsigned int n);

#endif
