#include "TrayIcon.h"

namespace toybox::standalone
{

void TrayIcon::showPlatformMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke)
{
	// Linux has no single native menu to call: the X11 tray protocol only transports the icon,
	// and a real desktop menu would need StatusNotifierItem + DBusMenu. Keep JUCE here.
	showJuceMenu (model, invoke);
}

} // namespace toybox::standalone
