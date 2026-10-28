#include <stdarg.h>
#include <stdint.h>

int64_t c_sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int64_t total = 0;
    for (int i = 0; i < n; ++i) { total += va_arg(ap, int64_t); }
    va_end(ap);
    return total;
}

double c_sum_f64(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double total = 0;
    for (int i = 0; i < n; ++i) { total += va_arg(ap, double); }
    va_end(ap);
    return total;
}

// VARIADICS.md §11: the native interop proof covers pointers too. va_arg
// reads the pointer and the helper dereferences it, so a caller that passed
// the value instead of its address cannot produce the expected sum.
int64_t c_sum_ptr(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int64_t total = 0;
    for (int i = 0; i < n; ++i) {
        total += *va_arg(ap, const int64_t*);
    }
    va_end(ap);
    return total;
}
