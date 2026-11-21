/* Tiny test framework for the host tests.
 *
 *   static void test_something(void) { CHECK(x); CHECK_EQ(a, b); }
 *   int main(void) { RUN(test_something); return test_summary(); }
 *
 * A failed check prints file:line and ends the current test. Define
 * TEST_SETUP() before including this file to run code before each test. */
#pragma once
#include <stdio.h>
#include <string.h>

static int t_failed, t_run, t_cur_failed;

#define CHECK(cond) do {                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            t_cur_failed = 1;                                                   \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b) do {                                                     \
        long long a_ = (long long)(a), b_ = (long long)(b);                     \
        if (a_ != b_) {                                                         \
            fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", \
                    __FILE__, __LINE__, #a, #b, a_, b_);                        \
            t_cur_failed = 1;                                                   \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b) do {                                                    \
        const char *a_ = (a), *b_ = (b);                                        \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                \
            fprintf(stderr, "  %s:%d: CHECK_STR(%s, %s) failed: \"%s\" != \"%s\"\n", \
                    __FILE__, __LINE__, #a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
            t_cur_failed = 1;                                                   \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_MEM(a, b, n) do {                                                 \
        if (memcmp((a), (b), (n)) != 0) {                                       \
            fprintf(stderr, "  %s:%d: CHECK_MEM(%s, %s, %s) failed\n",          \
                    __FILE__, __LINE__, #a, #b, #n);                            \
            t_cur_failed = 1;                                                   \
            return;                                                             \
        }                                                                       \
    } while (0)

#ifndef TEST_SETUP
#define TEST_SETUP() do { } while (0)
#endif

#define RUN(fn) do {                                                            \
        t_cur_failed = 0;                                                       \
        TEST_SETUP();                                                           \
        fn();                                                                   \
        t_run++;                                                                \
        if (t_cur_failed) { t_failed++; printf("FAIL %s\n", #fn); }             \
        else              { printf("ok   %s\n", #fn); }                         \
    } while (0)

static inline int test_summary(void)
{
    printf("%d tests, %d failed\n", t_run, t_failed);
    return t_failed ? 1 : 0;
}
