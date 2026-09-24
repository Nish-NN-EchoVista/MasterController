/* Minimal test harness: no dependencies beyond the C library. */
#ifndef MC_TEST_H
#define MC_TEST_H

#include <stdio.h>
#include <string.h>

extern int mc_test_failures;
extern int mc_test_checks;

#define CHECK(cond) do { ++mc_test_checks; if (!(cond)) { ++mc_test_failures; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_STR(actual, expected) do { ++mc_test_checks; \
    const char *a_ = (actual), *e_ = (expected); \
    if (strcmp(a_, e_) != 0) { ++mc_test_failures; \
    printf("  FAIL %s:%d:\n    got      \"%s\"\n    expected \"%s\"\n", \
           __FILE__, __LINE__, a_, e_); } } while (0)

#define CHECK_MEM(actual, alen, expected) do { ++mc_test_checks; \
    const char *e_ = (expected); size_t el_ = strlen(e_); \
    if ((size_t)(alen) != el_ || memcmp((actual), e_, el_) != 0) { ++mc_test_failures; \
    printf("  FAIL %s:%d:\n    got      \"%.*s\"\n    expected \"%s\"\n", \
           __FILE__, __LINE__, (int)(alen), (const char *)(actual), e_); } } while (0)

#define RUN(fn) do { printf("%s\n", #fn); fn(); } while (0)

#endif
