#ifndef _MATH_H
#define _MATH_H

double ldexp(double x, int exp);
long double ldexpl(long double x, int exp);

#define HUGE_VAL  (__builtin_huge_val())

#endif
