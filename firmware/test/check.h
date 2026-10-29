/* A minimal test harness: each test file has a main() that runs its cases. */
#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>

static int check_failures, check_count;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		check_count++;                                                         \
		if (!(cond)) {                                                         \
			check_failures++;                                              \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		}                                                                      \
	} while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
	do {                                                                           \
		double a_ = (a), b_ = (b);                                             \
		check_count++;                                                         \
		if (!(fabs(a_ - b_) <= (tol))) {                                       \
			check_failures++;                                              \
			fprintf(stderr, "%s:%d: %s = %g, want %g +- %g\n", __FILE__, __LINE__, #a, a_, b_, (double) (tol)); \
		}                                                                      \
	} while (0)

#define CHECK_STR(a, b)                                                                \
	do {                                                                           \
		const char *a_ = (a), *b_ = (b);                                       \
		check_count++;                                                         \
		if (!a_ || !b_ || strcmp(a_, b_)) {                                    \
			check_failures++;                                              \
			fprintf(stderr, "%s:%d: %s\n  got  %s\n  want %s\n", __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
		}                                                                      \
	} while (0)

#define CHECK_HAS(hay, needle)                                                         \
	do {                                                                           \
		const char *h_ = (hay), *n_ = (needle);                                \
		check_count++;                                                         \
		if (!strstr(h_, n_)) {                                                 \
			check_failures++;                                              \
			fprintf(stderr, "%s:%d: missing %s in\n  %s\n", __FILE__, __LINE__, n_, h_); \
		}                                                                      \
	} while (0)

static inline int check_report(const char *name) {
	printf("%-12s %d checks, %d failed\n", name, check_count, check_failures);
	return check_failures ? 1 : 0;
}
