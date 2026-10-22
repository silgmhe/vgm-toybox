#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined (_WIN32)
	#include <process.h>
#else
	#include <unistd.h>
#endif

namespace toybox::test
{

/** A settings folder for one test run, removed when the run ends.

	The store reads its location once, on first use, from `VGMTOYBOX_SETTINGS_DIR`
	(`SettingsStore::settingsDirectory()`), so setting the variable before anything constructs the
	store is all the isolation there is. Every test executable declares one of these first thing in
	`main()`:

		const toybox::test::ScopedSettingsDirectory settings;

	Why this matters. A test that reads the machine's own `settings.xml` does not test the code, it
	tests the preferences of whoever runs it. That is not hypothetical: `pitch.useSampleRoot` was left
	on after an experiment, the notes a sampled chip reports moved by up to two octaves, and six
	`engine.*` tests went red with no code change at all. It looked like a pitch bug for a while, and
	the first thing tried was editing the real AppData file - which is the situation this header
	exists to remove.

	Two rules follow:

	  1. Never write to the real folder from a test, and never require someone to edit it to get a
	     green run.
	  2. A test that needs a non-default preference sets it on the store *after* this constructor, so
	     it lands in the throwaway file and the run stays repeatable on any machine.

	Deliberately free of JUCE: `vgm_chip_test` links only the decoders, and it has to be able to use
	this too (a library it does not link is a compile error, not an unused include).
*/
struct ScopedSettingsDirectory
{
	std::filesystem::path directory;

	ScopedSettingsDirectory()
	{
		// unique per process, so two test executables running at once cannot see each other's writes
		static unsigned counter = 0;
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	  #if defined (_WIN32)
		const long pid = (long) _getpid();
	  #else
		const long pid = (long) getpid();
	  #endif
		const std::string name = "vgmtoybox-test-settings-" + std::to_string (pid) + "-"
								 + std::to_string (stamp) + "-" + std::to_string (counter++);

		directory = std::filesystem::temp_directory_path() / name;
		std::error_code error;
		std::filesystem::create_directories (directory, error);

		const std::string path = directory.string();
	  #if defined (_WIN32)
		_putenv_s ("VGMTOYBOX_SETTINGS_DIR", path.c_str());
	  #else
		::setenv ("VGMTOYBOX_SETTINGS_DIR", path.c_str(), 1);
	  #endif
	}

	~ScopedSettingsDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all (directory, error);
	}

	ScopedSettingsDirectory (const ScopedSettingsDirectory&) = delete;
	ScopedSettingsDirectory& operator= (const ScopedSettingsDirectory&) = delete;
};

} // namespace toybox::test
