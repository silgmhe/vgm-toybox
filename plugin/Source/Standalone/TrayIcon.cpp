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

namespace
{
	class JuceMenuRenderer final : public TrayMenuRenderer
	{
	public:
		explicit JuceMenuRenderer (juce::Colour colour) : coreColour (colour) {}

		void addSeparator (void) override
		{
			current().addSeparator();
		}

		void addItem (const TrayMenuEntry& entry) override
		{
			juce::PopupMenu::Item item;
			item.itemID = (int) ids.size() + 1;
			item.text = entry.label;
			item.isEnabled = entry.enabled;
			item.isTicked = entry.ticked;
			item.image = makeIcon (entry.icon, entry.enabled);
			current().addItem (std::move (item));
			ids.push_back (entry.id);
		}

		void beginSubmenu (const TrayMenuGroup& group) override
		{
			stack.emplace_back();
			groups.push_back (&group);
		}

		void endSubmenu (void) override
		{
			auto submenu = std::move (stack.back());
			stack.pop_back();

			const auto* group = groups.back();
			groups.pop_back();

			auto icon = makeIcon (group->icon, true);

			if (icon != nullptr)
			{
				current().addSubMenu (group->title, submenu, true, std::move (icon));
			}
			else
			{
				current().addSubMenu (group->title, submenu);
			}
		}

		juce::PopupMenu menu;
		std::vector<juce::String> ids;

	private:
		juce::PopupMenu& current (void)
		{
			return stack.empty() ? menu : stack.back();
		}

		std::unique_ptr<juce::Drawable> makeIcon (TrayMenuIcon icon, bool enabled) const
		{
			if (icon == TrayMenuIcon::none)
			{
				return {};
			}

			const auto image = makeTrayMenuIcon (icon, 64, coreColour, enabled ? 1.0f : 0.5f);

			if (! image.isValid())
			{
				return {};
			}

			auto drawable = std::make_unique<juce::DrawableImage>();
			drawable->setImage (image);
			return std::unique_ptr<juce::Drawable> (std::move (drawable));
		}

		juce::Colour coreColour;
		std::vector<juce::PopupMenu> stack;
		std::vector<const TrayMenuGroup*> groups;
	};
}

bool TrayIcon::show (const juce::String& tooltip,
					 std::function<void()> onActivate,
					 TrayMenuCallbacks menu)
{
	activate = std::move (onActivate);
	menuCallbacks = std::move (menu);

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
	if (menuCallbacks.build == nullptr)
	{
		return;
	}

	showPlatformMenu (menuCallbacks.build(), menuCallbacks.invoke);
}

void TrayIcon::showJuceMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke)
{
	JuceMenuRenderer renderer (findColour (juce::PopupMenu::textColourId));
	renderTrayMenu (model, renderer);

	if (renderer.ids.empty())
	{
		return;
	}

	// Linux is the only platform that reaches this in normal operation. macOS calls it only if
	// the status item has no native handle, and Windows only if its popup menu cannot be created.
	renderer.menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
								 [ids = std::move (renderer.ids), invoke] (int result)
								 {
									 if (result <= 0 || (size_t) result > ids.size())
									 {
										 return;
									 }

									 if (invoke != nullptr)
									 {
										 invoke (ids[(size_t) result - 1]);
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
				// White is the operating system's contract for a template image, not the
				// application's colour: the shell inverts this silhouette for either taskbar.
				static constexpr juce::uint32 kTemplateInk = 0xffffffff;
				image.setPixelAt (x, y, juce::Colour (kTemplateInk).withAlpha (pixel.getAlpha()));
			}
		}
	}

	return image;
}

} // namespace toybox::standalone
