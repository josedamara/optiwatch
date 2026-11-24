#include "test.h"

int t_failures;
int t_checks;

static int run_suite(const char *suite, const struct test_case *tests)
{
	int failed_tests = 0;
	for (const struct test_case *t = tests; t->name; t++) {
		int before = t_failures;
		t->fn();
		int ok = t_failures == before;
		printf("[%s] %s.%s\n", ok ? " OK " : "FAIL", suite, t->name);
		failed_tests += !ok;
	}
	return failed_tests;
}

int main(void)
{
	int failed = 0;
	failed += run_suite("sff8472", sff8472_tests);
	failed += run_suite("cmis", cmis_tests);
	failed += run_suite("bus", bus_tests);
	printf("\n%d checks, %d failed assertions, %d failed tests\n", t_checks, t_failures, failed);
	return failed ? 1 : 0;
}
