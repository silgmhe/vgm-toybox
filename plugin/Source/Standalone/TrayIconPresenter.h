#pragma once

#include <functional>

#include <juce_core/juce_core.h>

#include "Standalone/TrayMenuModel.h"

namespace toybox::standalone
{

/** What the presenter may do with the menu: build a fresh model when the menu opens, and invoke
	the selected id after the menu has closed. Platform adapters never run entry actions directly;
	that keeps enabled checks and dispatch in TrayMenu, where the console test can cover them. */
struct TrayMenuCallbacks
{
	std::function<TrayMenuModel()> build;
	TrayMenuInvoker invoke;
};

/**
	The thing that actually owns the notification-area icon.

	Kept behind an interface so the close-to-tray policy can be tested without a desktop: the
	console test supplies a fake that records which window callback ran and keeps the build/invoke
	callbacks, so the test drives the same menu path as a platform adapter. The application
	supplies TrayIcon.cpp, which puts a juce::SystemTrayIconComponent on the real notification
	area.
*/
class TrayIconPresenter
{
public:
	virtual ~TrayIconPresenter() = default;

	/** Puts the icon on the notification area. False when this build cannot show one; the
		window is then left open and the close goes on to quit as it always did. */
	virtual bool show (const juce::String& tooltip,
					   std::function<void()> onActivate,
					   TrayMenuCallbacks menu) = 0;

	/** Takes the icon away. Safe to call when it is not showing. */
	virtual void hide (void) = 0;
};

} // namespace toybox::standalone
