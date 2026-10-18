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

	if (bitmap == nullptr)
	{
		return nullptr;
	}

	if (pixels == nullptr)
	{
		DeleteObject (bitmap);
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
	WM_COMMAND. Command ids index stable model ids directly, and the controller invokes the
	selected id after the menu has been destroyed. The menu uses the system font, shadow,
	dark-mode colours and DPI scaling; none of that is JUCE-drawn.
*/
class WindowsMenuRenderer final : public TrayMenuRenderer
{
public:
	WindowsMenuRenderer (HMENU rootMenu, juce::Colour colour, int pixelSize)
		: root (rootMenu), coreColour (colour), iconPixelSize (pixelSize)
	{
	}

	~WindowsMenuRenderer (void) override
	{
		clearBitmaps();
	}

	void addSeparator (void) override
	{
		if (auto* into = current())
		{
			AppendMenuW (into, MF_SEPARATOR, 0, nullptr);
		}
	}

	void addItem (const TrayMenuEntry& entry) override
	{
		auto* into = current();

		if (into == nullptr)
		{
			return;
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

		const UINT id = static_cast<UINT> (ids.size() + 1);

		if (AppendMenuW (into, flags, id, entry.label.toWideCharPointer()))
		{
			ids.push_back (entry.id);
			attachIcon (into, GetMenuItemCount (into) - 1, entry.icon, entry.enabled);
		}
	}

	void beginSubmenu (const TrayMenuGroup& group) override
	{
		stack.push_back (CreatePopupMenu());
		groups.push_back (&group);
	}

	void endSubmenu (void) override
	{
		HMENU submenu = stack.back();
		stack.pop_back();

		const auto* group = groups.back();
		groups.pop_back();

		auto* parent = current();

		if (submenu == nullptr || parent == nullptr)
		{
			if (submenu != nullptr)
			{
				DestroyMenu (submenu);
			}

			return;
		}

		if (AppendMenuW (parent, MF_POPUP | MF_STRING,
						 reinterpret_cast<UINT_PTR> (submenu),
						 group->title.toWideCharPointer()))
		{
			attachIcon (parent, GetMenuItemCount (parent) - 1, group->icon, true);
		}
		else
		{
			DestroyMenu (submenu);
		}
	}

	UINT defaultItemId (void) const
	{
		for (std::size_t i = 0; i < ids.size(); ++i)
		{
			if (ids[i] == "tray.show")
			{
				return static_cast<UINT> (i + 1);
			}
		}

		return 0;
	}

	void clearBitmaps (void)
	{
		for (auto bitmap : bitmaps)
		{
			DeleteObject (bitmap);
		}

		bitmaps.clear();
	}

	std::vector<juce::String> ids;

private:
	HMENU current (void) const
	{
		return stack.empty() ? root : stack.back();
	}

	void attachIcon (HMENU into, int index, TrayMenuIcon icon, bool enabled)
	{
		if (icon == TrayMenuIcon::none || index < 0)
		{
			return;
		}

		const auto image = makeTrayMenuIcon (icon, iconPixelSize, coreColour,
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
	}

	HMENU root;
	juce::Colour coreColour;
	int iconPixelSize;
	std::vector<HMENU> stack;
	std::vector<const TrayMenuGroup*> groups;
	std::vector<HBITMAP> bitmaps;
};

bool showWindowsMenu (HWND owner, const TrayMenuModel& model, const TrayMenuInvoker& invoke)
{
	HMENU menu = CreatePopupMenu();

	if (menu == nullptr)
	{
		return false;
	}

	WindowsMenuRenderer renderer (menu,
								  windowsColour (COLOR_MENU).contrasting(),
								  menuIconPixelSize (owner));
	renderTrayMenu (model, renderer);

	// Enter on the menu shows the window, the least destructive action. Resolve it by id rather
	// than by position: a new group before Show must not silently change the default row.
	const UINT defaultId = renderer.defaultItemId();

	if (defaultId != 0)
	{
		SetMenuDefaultItem (menu, defaultId, FALSE);
	}

	POINT position {};
	GetCursorPos (&position);
	SetForegroundWindow (owner);

	const UINT command = TrackPopupMenuEx (menu,
										 TPM_RETURNCMD | TPM_RIGHTBUTTON,
										 position.x, position.y,
										 owner, nullptr);

	const juce::String selectedId = command > 0 && command <= renderer.ids.size()
		? renderer.ids[command - 1]
		: juce::String();

	DestroyMenu (menu);
	renderer.clearBitmaps();
	PostMessage (owner, WM_NULL, 0, 0);	// lets the menu close if focus moved elsewhere

	if (! selectedId.isEmpty() && invoke != nullptr)
	{
		invoke (selectedId);
	}

	return true;
}
} // namespace
#endif

void TrayIcon::showPlatformMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke)
{
   #if JUCE_WINDOWS
	if (auto* owner = reinterpret_cast<HWND> (getWindowHandle()))
	{
		if (showWindowsMenu (owner, model, invoke))
		{
			return;
		}
	}
   #endif

	showJuceMenu (model, invoke);
}

} // namespace toybox::standalone
