#include "TrayController.h"

#include <utility>

namespace toybox::standalone
{

TrayController::TrayController (PlaybackEngine& engine,
								settings::SettingsStore& settingsStore,
								std::function<void()> hideWindowCallback,
								std::function<void()> showWindowCallback,
								std::function<void()> quitApplicationCallback,
								IconFactory iconFactory)
	: settings (settingsStore),
	  hideWindow (std::move (hideWindowCallback)),
	  showWindow (std::move (showWindowCallback)),
	  quitApplication (std::move (quitApplicationCallback)),
	  createIcon (std::move (iconFactory)),
	  menu (engine,
			[this] { activate(); },
			[this]
			{
				// Every adapter invokes the chosen entry after its menu has closed: Win32 after
				// TrackPopupMenuEx returns, macOS after popUpStatusItemMenu: returns, JUCE from
				// showMenuAsync's result callback. Removing the icon here therefore cannot pull
				// the menu out from under the click that chose Quit.
				hideIcon();

				if (quitApplication != nullptr)
				{
					quitApplication();
				}
			})
{
}

TrayController::~TrayController (void)
{
	hideIcon();
}

bool TrayController::closeRequested (void)
{
	if (! settings.getCloseToTray())
	{
		return false;
	}

	// A build with no tray backend leaves the window to close normally. Hiding it without an
	// icon to bring it back would be the one failure the user cannot recover from.
	if (! showIcon())
	{
		return false;
	}

	if (hideWindow != nullptr)
	{
		hideWindow();
	}

	return true;
}

void TrayController::activate (void)
{
	if (showWindow != nullptr)
	{
		showWindow();
	}
}

bool TrayController::isIconVisible (void) const
{
	return iconVisible;
}

const TrayMenu& TrayController::getMenu (void) const
{
	return menu;
}

bool TrayController::showIcon (void)
{
	if (iconVisible)
	{
		return true;
	}

	if (icon == nullptr)
	{
		if (createIcon == nullptr)
		{
			return false;
		}

		icon = createIcon();

		if (icon == nullptr)
		{
			return false;
		}
	}

	iconVisible = icon->show ("VGMToybox",
							  [this] { activate(); },
							  { [this] { return menu.build(); },
								[this] (const juce::String& id)
								{
									return menu.invoke (id);
								} });

	if (! iconVisible)
	{
		// Let the next close try again rather than keeping a presenter that cannot put
		// anything on screen.
		icon.reset();
	}

	return iconVisible;
}

void TrayController::hideIcon (void)
{
	if (icon != nullptr && iconVisible)
	{
		icon->hide();
	}

	iconVisible = false;
}

} // namespace toybox::standalone
