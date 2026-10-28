#include <stdio.h>

static int add(int a, int b) {
    return a + b;
}

int main(void) {
    int i;
    int s = 0;
    long long n = 10000000000LL;
    FILE *f;

    for (i = 1; i <= 100; i++) {
        s = add(s, i);
    }
    n = n / 10;

    f = fopen("TCC.OUT", "w");
    if (!f) {
        printf("hello from tcc open fail\n");
        return 1;
    }
    fprintf(f, "sum %d\n", s);
    fclose(f);
    printf("hello from tcc %d %lld\n", s, n);
    return 0;
}
