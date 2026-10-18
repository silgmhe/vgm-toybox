/*
    The one piece of shared plumbing for the console test programs.

    Each test used to print its own " OK /FAIL" line and count failures in a file-local variable.
    That is still all this does - but a failure now also says which file and line produced it, and
    the counter is shared within the program so `main` does not need its own copy.

    It deliberately stops there: no test registry, no fixtures, no assertion vocabulary beyond a
    bool. docs/desgin/testing-architecture.md explains why; CTest (CMakeLists.txt) is the runner
    that supplies names, filtering and the JUnit report.

    The `check` macro keeps the existing call sites - `check (condition, "sentence")` - working
    unchanged, so the ~200 checks did not have to move.
*/

#pragma once

#include <cstdio>

#include <juce_core/juce_core.h>

namespace toybox::test
{
    inline int& failures()
    {
        static int count = 0;
        return count;
    }

    inline void report (bool ok, const juce::String& what, const char* file, int line)
    {
        if (ok)
        {
            std::printf ("    %s %s\n", " OK ", what.toRawUTF8());
        }
        else
        {
            std::printf ("    FAIL %s  (%s:%d)\n", what.toRawUTF8(), file, line);
            ++failures();
        }
    }
}

#define check(condition, message) \
    ::toybox::test::report ((condition), (message), __FILE__, __LINE__)
