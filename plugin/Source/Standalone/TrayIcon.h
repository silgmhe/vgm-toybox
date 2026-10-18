#pragma once

#include <functional>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Standalone/TrayIconPresenter.h"

namespace toybox::standalone
{

/**
	The notification-area icon, on the real notification area.

	This is the only file in the tray feature that knows what a desktop is; everything above it
	(TrayController, TrayMenu) is compiled into the console test as well. A left click calls the
	controller's show-window callback; a right click turns the current menu model into a menu and
	runs it. The adapter only maps native command ids back to the model's stable ids; the controller
	invokes the action, so enabled checks and dispatch stay testable and this class stays thin.

	On Windows the menu is a real Win32 popup menu (system font, dark mode, DPI scaling); on macOS
	it is an NSMenu attached to the NSStatusItem's button. Linux keeps JUCE's menu because a native
	menu there means a different desktop protocol (StatusNotifierItem + DBusMenu), not a toolkit
	widget. The icon itself is the application icon in every case; the macOS template is derived
	from the same drawing.
*/
class TrayIcon final : public juce::SystemTrayIconComponent,
					   public TrayIconPresenter
{
public:
	TrayIcon (void) = default;
	~TrayIcon (void) override = default;

	bool show (const juce::String& tooltip,
			   std::function<void()> onActivate,
			   TrayMenuCallbacks menu) override;

	void hide (void) override;

private:
	void mouseDown (const juce::MouseEvent& event) override;
	void showMenu (void);

	/** The platform menu adapter. Defined once per platform in TrayIcon_windows.cpp,
		TrayIcon_mac.mm or TrayIcon_linux.cpp. */
	void showPlatformMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke);

	/** The toolkit menu used by Linux, and as a fallback if the native handle is missing. */
	void showJuceMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke);

	static juce::Image makeAppIcon (void);
	static juce::Image makeTemplateImage (const juce::Image& colourIcon);

	std::function<void()> activate;
	TrayMenuCallbacks menuCallbacks;
};

} // namespace toybox::standalone
