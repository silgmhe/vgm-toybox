#pragma once

#include <functional>
#include <memory>

#include <juce_core/juce_core.h>

#include "Standalone/TrayIconPresenter.h"
#include "Standalone/TrayMenu.h"
#include "playback/PlaybackEngine.h"
#include "settings/SettingsStore.h"

namespace toybox::standalone
{

/**
	Decides what a window close means, and what the tray icon does.

	Owned by the standalone's window (Source/Standalone/StandaloneApp.cpp). It is deliberately
	free of GUI code so that this policy - the part that can strand a user - is covered by the
	console test rather than only by a manual click on a real tray icon.

	The icon is created by the first close that is swallowed and then stays until the application
	quits, even while the window is back on screen: that is what makes "the application is still
	running" visible, and it is also the only shape the macOS menu-bar API supports without
	leaving an empty status item behind. The menu itself is rebuilt on every right-click; nothing
	here caches play state.
*/
class TrayController
{
public:
	using IconFactory = std::function<std::unique_ptr<TrayIconPresenter>()>;

	TrayController (PlaybackEngine& engine,
					settings::SettingsStore& settings,
					std::function<void()> hideWindow,
					std::function<void()> showWindow,
					std::function<void()> quitApplication,
					IconFactory createIcon);

	~TrayController (void);

	/** The window's close button went down. True when the close was swallowed: the icon is up
		and the window has been hidden. False when this close should go on and quit.

		Reads the setting now, not in the constructor, so the switch in the General page applies
		to the next close without restarting the application. */
	bool closeRequested (void);

	/** The icon was clicked with the left button: put the window back. */
	void activate (void);

	bool isIconVisible (void) const;

	/** Read-only view for tests. Production reaches the menu through the callbacks passed to the
		presenter, so nothing outside needs a mutable model. */
	const TrayMenu& getMenu (void) const;

private:
	bool showIcon (void);
	void hideIcon (void);

	settings::SettingsStore& settings;
	std::function<void()> hideWindow;
	std::function<void()> showWindow;
	std::function<void()> quitApplication;
	IconFactory createIcon;

	std::unique_ptr<TrayIconPresenter> icon;
	bool iconVisible = false;
	TrayMenu menu;
};

} // namespace toybox::standalone
