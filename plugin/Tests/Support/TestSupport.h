#pragma once

namespace toybox::test
{

/** Records one assertion and prints it.

	Shared by every test translation unit, so a new file needs no copy of the harness and the
	summary line keeps one format. */
void check (bool ok, const char* what);

/** Prints the summary line and returns the process exit code (0 when nothing failed). */
int report (void);

} // namespace toybox::test
