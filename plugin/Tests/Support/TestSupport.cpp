#include "TestSupport.h"

#include <cstdio>

namespace toybox::test
{
namespace
{
	int failures = 0;
}

void check (bool ok, const char* what)
{
	std::printf (" %s  %s\n", ok ? "OK  " : "FAIL", what);

	if (! ok)
	{
		++failures;
	}
}

int report (void)
{
	std::printf ("Result    : %s\n", failures == 0 ? "PASS" : "FAILURES");
	return failures == 0 ? 0 : 1;
}

} // namespace toybox::test
