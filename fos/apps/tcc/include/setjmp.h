#ifndef _SETJMP_H
#define _SETJMP_H

typedef long jmp_buf[32];

#if defined(__GNUC__) && !defined(__TINYC__)
int setjmp(jmp_buf env) __attribute__((returns_twice));
#else
int setjmp(jmp_buf env);
#endif
void longjmp(jmp_buf env, int val);

#endif
