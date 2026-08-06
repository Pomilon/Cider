#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

int main(void) {
    int N = 2000000;
    bool* sieve = malloc(N);
    for (int i = 0; i < N; i++) sieve[i] = true;
    sieve[0] = sieve[1] = false;
    for (int p = 2; p * p < N; p++) {
        if (sieve[p]) {
            for (int j = p * p; j < N; j += p) sieve[j] = false;
        }
    }
    int count = 0;
    for (int i = 0; i < N; i++) if (sieve[i]) count++;
    printf("%d", count);
    free(sieve);
    return 0;
}
