#include "TrayIcon.h"
#include "TrayMenuIcons.h"

#include "TrayIconData.h"

#include <memory>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace toybox::standalone
{

bool TrayIcon::show (const juce::String& tooltip,
					 std::function<void()> onActivate,
					 std::function<TrayMenuModel()> menuBuilder)
{
	activate = std::move (onActivate);
	buildMenu = std::move (menuBuilder);

	const auto icon = makeAppIcon();
	if (! icon.isValid())
	{
		return false;	// no icon means no hiding: the controller keeps the close quit path
	}

	setIconImage (icon, makeTemplateImage (icon));
	setIconTooltip (tooltip);

	return true;
}

void TrayIcon::hide (void)
{
	setIconImage ({}, {});
}

void TrayIcon::mouseDown (const juce::MouseEvent& event)
{
	if (event.mods.isPopupMenu())
	{
		showMenu();
		return;
	}

	if (activate != nullptr)
	{
		activate();
	}
}

void TrayIcon::showMenu (void)
{
	if (buildMenu == nullptr)
	{
		return;
	}

	showPlatformMenu (buildMenu());
}

void TrayIcon::showJuceMenu (const TrayMenuModel& model)
{
	juce::PopupMenu menu;
	std::vector<std::function<void()>> actions;
	int nextId = 1;
	bool hasEntries = false;

	const auto coreColour = findColour (juce::PopupMenu::textColourId);

	auto makeIcon = [coreColour] (TrayMenuIcon icon, bool ticked, bool enabled)
	{
		if (icon == TrayMenuIcon::none)
		{
			return std::unique_ptr<juce::Drawable>();
		}

		const auto image = makeTrayMenuIcon (icon, 64, ticked, coreColour,
											 enabled ? 1.0f : 0.5f);

		if (! image.isValid())
		{
			return std::unique_ptr<juce::Drawable>();
		}

		auto drawable = std::make_unique<juce::DrawableImage>();
		drawable->setImage (image);
		return std::unique_ptr<juce::Drawable> (std::move (drawable));
	};

	// Item ids index actions directly (id - 1), which works because both counters advance
	// together and submenu items are appended in the order the model lists them.
	auto addEntries = [&] (juce::PopupMenu& into, const TrayMenuGroup& group)
	{
		for (const auto& entry : group.entries)
		{
			if (entry.separatorBefore)
			{
				into.addSeparator();
			}

			juce::PopupMenu::Item item;
			item.itemID = nextId++;
			item.text = entry.label;
			item.isEnabled = entry.enabled;
			item.isTicked = entry.ticked;
			item.image = makeIcon (entry.icon, entry.ticked, entry.enabled);
			into.addItem (std::move (item));

			actions.push_back (entry.enabled ? entry.action : std::function<void()>());
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

		juce::PopupMenu submenu;
		addEntries (submenu, group);

		if (hasEntries)
		{
			menu.addSeparator();
		}

		auto submenuIcon = makeIcon (group.icon, false, true);

		if (submenuIcon != nullptr)
		{
			menu.addSubMenu (group.title, submenu, true, std::move (submenuIcon));
		}
		else
		{
			menu.addSubMenu (group.title, submenu);
		}

		hasEntries = true;
	}

	// Linux is the only platform that reaches this in normal operation. macOS calls it only if
	// the status item has no native handle, and Windows only if its popup menu cannot be created.
	menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
						[actions = std::move (actions)] (int result)
						{
							if (result <= 0 || (size_t) result > actions.size())
							{
								return;
							}

							if (auto& action = actions[(size_t) result - 1])
							{
								action();
							}
						});
}

juce::Image TrayIcon::makeAppIcon (void)
{
   #if JUCE_WINDOWS
	const int target = juce::jlimit (16, 64, (int) GetSystemMetrics (SM_CXSMICON));
	const char* resource = (target <= 16) ? "vgmtoybox16_png" : "vgmtoybox256_png";
   #else
	const int target = 32;
	const char* resource = "vgmtoybox256_png";
   #endif

	int size = 0;
	const auto* data = TrayIconData::getNamedResource (resource, size);
	if (data == nullptr || size <= 0)
	{
		return {};
	}

	auto image = juce::ImageFileFormat::loadFrom (data, (size_t) size);
	if (! image.isValid())
	{
		return {};
	}

	if (image.getWidth() != target || image.getHeight() != target)
	{
		image = image.rescaled (target, target, juce::Graphics::highResamplingQuality);
	}

	return image;
}

juce::Image TrayIcon::makeTemplateImage (const juce::Image& colourIcon)
{
	// macOS renders the template's alpha as the menu-bar foreground colour. Keep the white chip
	// and pins from the application icon and let its blue tile disappear, so the menu-bar mark is
	// the same drawing as the window icon rather than a second, unrelated glyph.
	if (! colourIcon.isValid())
	{
		return {};
	}

	const auto source = colourIcon.rescaled (64, 64, juce::Graphics::highResamplingQuality);
	juce::Image image (juce::Image::ARGB, 64, 64, true);

	for (int y = 0; y < 64; ++y)
	{
		for (int x = 0; x < 64; ++x)
		{
			const auto pixel = source.getPixelAt (x, y);
			if (pixel.getAlpha() > 32
				&& pixel.getRed() > 180
				&& pixel.getGreen() > 180
				&& pixel.getBlue() > 180)
			{
				image.setPixelAt (x, y, juce::Colours::white.withAlpha (pixel.getAlpha()));
			}
		}
	}

	return image;
}

} // namespace toybox::standalone
