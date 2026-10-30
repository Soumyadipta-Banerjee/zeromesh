/* Smallest real TU for the local split-compile smoke test. */
#include "hello.h"

#ifndef GREETING
#error "GREETING must come from -D (the preprocessor step must apply it)"
#endif

int hello_value(void) {
    return HELLO_VALUE;
}
