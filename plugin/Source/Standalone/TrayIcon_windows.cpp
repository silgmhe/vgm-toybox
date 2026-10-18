#include "TrayIcon.h"
#include "TrayMenuIcons.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

#include <vector>

namespace toybox::standalone
{
#if JUCE_WINDOWS
namespace
{
juce::Colour windowsColour (int index)
{
	const auto colour = GetSysColor (index);
	return juce::Colour::fromRGB (GetRValue (colour), GetGValue (colour), GetBValue (colour));
}

int menuIconPixelSize (HWND owner)
{
	using GetDpiForWindowFn = UINT (WINAPI*) (HWND);
	using GetSystemMetricsForDpiFn = int (WINAPI*) (int, UINT);

	static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn> (
		GetProcAddress (GetModuleHandleW (L"user32.dll"), "GetDpiForWindow"));
	static const auto getSystemMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpiFn> (
		GetProcAddress (GetModuleHandleW (L"user32.dll"), "GetSystemMetricsForDpi"));

	const auto dpi = getDpiForWindow != nullptr ? getDpiForWindow (owner) : 96u;
	const auto metric = getSystemMetricsForDpi != nullptr
		? getSystemMetricsForDpi (SM_CXSMICON, dpi)
		: GetSystemMetrics (SM_CXSMICON);

	return juce::jlimit (16, 48, metric > 0 ? metric : 16);
}

/** A 32-bit top-down DIB with premultiplied alpha, which is what the themed menu renderer
	expects from an item bitmap. BI_RGB (not BI_BITFIELDS) is required here: the menu renderer
	only treats a 32-bit section as alpha-blended in that form; with explicit colour masks it
	draws the bitmap opaquely, which turns every transparent pixel into the black plate that
	the icons are designed not to have. */
HBITMAP createMenuBitmap (const juce::Image& image)
{
	if (! image.isValid())
	{
		return nullptr;
	}

	BITMAPINFO header = {};
	header.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
	header.bmiHeader.biWidth = image.getWidth();
	header.bmiHeader.biHeight = -image.getHeight();
	header.bmiHeader.biPlanes = 1;
	header.bmiHeader.biBitCount = 32;
	header.bmiHeader.biCompression = BI_RGB;

	void* pixels = nullptr;
	HBITMAP bitmap = CreateDIBSection (nullptr, &header, DIB_RGB_COLORS, &pixels, nullptr, 0);

	if (bitmap == nullptr || pixels == nullptr)
	{
		return nullptr;
	}

	auto* destination = static_cast<uint32_t*> (pixels);

	for (int y = 0; y < image.getHeight(); ++y)
	{
		for (int x = 0; x < image.getWidth(); ++x)
		{
			const auto colour = image.getPixelAt (x, y);
			const auto alpha = colour.getAlpha();
			const auto red = (colour.getRed() * alpha + 127) / 255;
			const auto green = (colour.getGreen() * alpha + 127) / 255;
			const auto blue = (colour.getBlue() * alpha + 127) / 255;

			destination[y * image.getWidth() + x] =
				(static_cast<uint32_t> (alpha) << 24)
				| (static_cast<uint32_t> (red) << 16)
				| (static_cast<uint32_t> (green) << 8)
				| static_cast<uint32_t> (blue);
		}
	}

	return bitmap;
}

/** A real Win32 popup menu.

	`TPM_RETURNCMD` makes TrackPopupMenuEx return the chosen command instead of posting
	WM_COMMAND, so the model's std::function still runs after the menu has been destroyed. The menu
	uses the system font, shadow, dark-mode colours and DPI scaling; none of that is JUCE-drawn.
*/
bool showWindowsMenu (HWND owner, const TrayMenuModel& model)
{
	HMENU menu = CreatePopupMenu();

	if (menu == nullptr)
	{
		return false;
	}

	std::vector<std::function<void()>> actions;
	std::vector<HBITMAP> bitmaps;
	bool hasEntries = false;

	// Derive the ink from the menu background rather than assuming a light menu: Windows can
	// hand us a dark popup in dark mode, where black glyphs would disappear.
	const auto coreColour = windowsColour (COLOR_MENU).contrasting();
	const auto iconPixelSize = menuIconPixelSize (owner);

	auto attachIcon = [&] (HMENU into, int index, TrayMenuIcon icon, bool ticked, bool enabled)
	{
		if (icon == TrayMenuIcon::none || index < 0)
		{
			return;
		}

		const auto image = makeTrayMenuIcon (icon, iconPixelSize, ticked, coreColour,
											 enabled ? 1.0f : 0.42f);
		HBITMAP bitmap = createMenuBitmap (image);

		if (bitmap == nullptr)
		{
			return;
		}

		MENUITEMINFOW info = {};
		info.cbSize = sizeof (info);
		info.fMask = MIIM_BITMAP;
		info.hbmpItem = bitmap;

		if (SetMenuItemInfoW (into, static_cast<UINT> (index), TRUE, &info))
		{
			bitmaps.push_back (bitmap);
		}
		else
		{
			DeleteObject (bitmap);
		}
	};

	// Command ids index actions directly (id - 1): both counters advance together, and a
	// separator advances neither.
	auto addEntries = [&] (HMENU into, const TrayMenuGroup& group)
	{
		for (const auto& entry : group.entries)
		{
			if (entry.separatorBefore)
			{
				AppendMenuW (into, MF_SEPARATOR, 0, nullptr);
			}

			UINT flags = MF_STRING;
			if (! entry.enabled)
			{
				flags |= MF_GRAYED;
			}
			if (entry.ticked)
			{
				flags |= MF_CHECKED;
			}

			const UINT id = static_cast<UINT> (actions.size() + 1);
			actions.push_back (entry.enabled ? entry.action : std::function<void()>());

			if (AppendMenuW (into, flags, id, entry.label.toWideCharPointer()))
			{
				attachIcon (into, GetMenuItemCount (into) - 1, entry.icon, entry.ticked,
							entry.enabled);
			}
		}
	};

	for (const auto& group : model)
	{
		if (group.title.isEmpty())
		{
			addEntries (menu, group);
			hasEntries = hasEntries || ! group.entries.empty();
			continue;
		}

		HMENU submenu = CreatePopupMenu();
		if (submenu == nullptr)
		{
			continue;
		}

		addEntries (submenu, group);

		if (hasEntries)
		{
			AppendMenuW (menu, MF_SEPARATOR, 0, nullptr);
		}

		if (AppendMenuW (menu, MF_POPUP | MF_STRING,
						 reinterpret_cast<UINT_PTR> (submenu), group.title.toWideCharPointer()))
		{
			attachIcon (menu, GetMenuItemCount (menu) - 1, group.icon, false, true);
		}

		hasEntries = true;
	}

	// Enter on the menu shows the window, the least destructive action.
	SetMenuDefaultItem (menu, 1, FALSE);

	POINT position {};
	GetCursorPos (&position);
	SetForegroundWindow (owner);

	const UINT command = TrackPopupMenuEx (menu,
										 TPM_RETURNCMD | TPM_RIGHTBUTTON,
										 position.x, position.y,
										 owner, nullptr);

	DestroyMenu (menu);

	for (auto bitmap : bitmaps)
	{
		DeleteObject (bitmap);
	}

	PostMessage (owner, WM_NULL, 0, 0);	// lets the menu close if focus moved elsewhere

	if (command > 0 && command <= static_cast<UINT> (actions.size()))
	{
		if (auto& action = actions[command - 1])
		{
			action();
		}
	}

	return true;
}
} // namespace
#endif

void TrayIcon::showPlatformMenu (const TrayMenuModel& model)
{
   #if JUCE_WINDOWS
	if (auto* owner = reinterpret_cast<HWND> (getWindowHandle()))
	{
		if (showWindowsMenu (owner, model))
		{
			return;
		}
	}
   #endif

	showJuceMenu (model);
}

} // namespace toybox::standalone
