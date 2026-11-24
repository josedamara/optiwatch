/* Minimal dependency-free unit test harness. */
#ifndef OPTIWATCH_TEST_H
#define OPTIWATCH_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int t_failures;
extern int t_checks;

#define CHECK(cond)                                                              \
	do {                                                                     \
		t_checks++;                                                      \
		if (!(cond)) {                                                   \
			t_failures++;                                            \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

#define CHECK_EQ_INT(a, b)                                                       \
	do {                                                                     \
		long long _a = (long long)(a), _b = (long long)(b);              \
		t_checks++;                                                      \
		if (_a != _b) {                                                  \
			t_failures++;                                            \
			fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
				__FILE__, __LINE__, #a, #b, _a, _b);             \
		}                                                                \
	} while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
	do {                                                                     \
		double _a = (double)(a), _b = (double)(b);                       \
		t_checks++;                                                      \
		if (!(fabs(_a - _b) <= (tol))) {                                 \
			t_failures++;                                            \
			fprintf(stderr, "  FAIL %s:%d: %s ~= %s (%g vs %g)\n",   \
				__FILE__, __LINE__, #a, #b, _a, _b);             \
		}                                                                \
	} while (0)

#define CHECK_STR(a, b)                                                          \
	do {                                                                     \
		const char *_a = (a), *_b = (b);                                 \
		t_checks++;                                                      \
		if (strcmp(_a, _b) != 0) {                                       \
			t_failures++;                                            \
			fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", \
				__FILE__, __LINE__, #a, _b, _a);                 \
		}                                                                \
	} while (0)

typedef void (*test_fn)(void);
struct test_case {
	const char *name;
	test_fn fn;
};

/* Each test file exports a NULL-terminated table. */
extern const struct test_case sff8472_tests[];
extern const struct test_case cmis_tests[];
extern const struct test_case bus_tests[];

/* Image builders shared by the tests (fixtures.c). */
#include <stdint.h>
void fx_put16(uint8_t *p, unsigned v);
void fx_put_ascii(uint8_t *p, const char *s, size_t width);
void fx_sfp_a0(uint8_t a0[256], int ext_cal);
void fx_sfp_a2(uint8_t a2[256]);
struct xcvr_bus;
void fx_cmis_fill(struct xcvr_bus *bus);

#endif
