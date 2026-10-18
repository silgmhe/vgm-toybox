#pragma once

#include <functional>
#include <memory>

#include <juce_core/juce_core.h>

#include "Standalone/TrayMenu.h"
#include "playback/PlaybackEngine.h"
#include "settings/SettingsStore.h"

namespace toybox::standalone
{

/**
	The thing that actually owns the notification-area icon.

	Kept behind an interface so the close-to-tray policy can be tested without a desktop: the
	console test supplies a fake that records which window callback ran and hands the menu back
	to the test. The application supplies TrayIcon.cpp, which puts a juce::SystemTrayIconComponent
	on the real notification area.
*/
class TrayIconPresenter
{
public:
	virtual ~TrayIconPresenter() = default;

	/** Puts the icon on the notification area. False when this build cannot show one; the
		window is then left open and the close goes on to quit as it always did. */
	virtual bool show (const juce::String& tooltip,
					   std::function<void()> onActivate,
					   std::function<TrayMenuModel()> buildMenu) = 0;

	/** Takes the icon away. Safe to call when it is not showing. */
	virtual void hide (void) = 0;
};

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
	TrayMenu& getMenu (void);

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
