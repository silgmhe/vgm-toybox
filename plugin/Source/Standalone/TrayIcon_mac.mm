#import <Cocoa/Cocoa.h>

#include "TrayIcon.h"
#include "TrayMenuIcons.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace toybox::standalone
{
/** The selection state is owned by showNativeMenu() for the duration of the modal menu. Menu
	items only record their tag here; the function invokes the stable id after AppKit has
	dismissed the menu, so a Quit cannot tear down the icon while the menu is still tracking.
*/
struct TrayNativeMenuState
{
	NSInteger selectedIndex = -1;
};
} // namespace toybox::standalone

@interface VGMToyboxTrayMenuTarget : NSObject
{
@public
	toybox::standalone::TrayNativeMenuState* state;
}

- (void)invoke:(NSMenuItem*)item;
@end

@implementation VGMToyboxTrayMenuTarget
- (void)invoke:(NSMenuItem*)item
{
	if (state != nullptr)
	{
		state->selectedIndex = item.tag;
	}
}
@end

namespace toybox::standalone
{
namespace
{
NSString* toNSString (const juce::String& text)
{
	return [NSString stringWithUTF8String: text.toRawUTF8()];
}

NSImage* makeItemImage (TrayMenuIcon icon)
{
	const auto source = makeTrayMenuIconTemplate (icon, 32);

	if (! source.isValid())
	{
		return nil;
	}

	juce::MemoryOutputStream stream;
	juce::PNGImageFormat png;

	if (! png.writeImageToStream (source, stream))
	{
		return nil;
	}

	NSData* data = [NSData dataWithBytes: stream.getData() size: stream.getDataSize()];
	NSImage* image = [[NSImage alloc] initWithData: data];

	if (image != nil)
	{
		[image setSize: NSMakeSize (16.0, 16.0)];
		[image setTemplate: YES];
	}

	return image;
}

void setItemImage (NSMenuItem* item, TrayMenuIcon icon)
{
	if (icon == TrayMenuIcon::none)
	{
		return;
	}

	if (NSImage* image = makeItemImage (icon))
	{
		[item setImage: image];
		[image release];
	}
}

class MacMenuRenderer final : public TrayMenuRenderer
{
public:
	MacMenuRenderer (NSMenu* rootMenu, VGMToyboxTrayMenuTarget* menuTarget)
		: root (rootMenu), target (menuTarget)
	{
	}

	void addSeparator (void) override
	{
		if (auto* into = current())
		{
			[into addItem: [NSMenuItem separatorItem]];
		}
	}

	void addItem (const TrayMenuEntry& entry) override
	{
		auto* into = current();

		if (into == nil)
		{
			return;
		}

		NSMenuItem* item = [[NSMenuItem alloc] initWithTitle: toNSString (entry.label)
													  action: @selector(invoke:)
											   keyEquivalent: @""];
		item.target = target;
		item.tag = static_cast<NSInteger> (ids.size());
		[item setEnabled: entry.enabled ? YES : NO];
		[item setState: entry.ticked ? NSControlStateValueOn : NSControlStateValueOff];
		setItemImage (item, entry.icon);
		[into addItem: item];
		[item release];

		ids.push_back (entry.id);
	}

	void beginSubmenu (const TrayMenuGroup& group) override
	{
		NSMenu* submenu = [[NSMenu alloc] initWithTitle: toNSString (group.title)];
		[submenu setAutoenablesItems: NO];
		stack.push_back (submenu);
		groups.push_back (&group);
	}

	void endSubmenu (void) override
	{
		NSMenu* submenu = stack.back();
		stack.pop_back();

		const auto* group = groups.back();
		groups.pop_back();

		auto* parent = current();

		if (submenu == nil || parent == nil)
		{
			[submenu release];
			return;
		}

		NSMenuItem* item = [[NSMenuItem alloc] initWithTitle: toNSString (group->title)
													  action: nil
											   keyEquivalent: @""];
		[item setSubmenu: submenu];
		setItemImage (item, group->icon);
		[parent addItem: item];
		[item release];
		[submenu release];
	}

	std::vector<juce::String> ids;

private:
	NSMenu* current (void) const
	{
		return stack.empty() ? root : stack.back();
	}

	NSMenu* root;
	VGMToyboxTrayMenuTarget* target;
	std::vector<NSMenu*> stack;
	std::vector<const TrayMenuGroup*> groups;
};

void showNativeMenu (NSStatusItem* statusItem,
					 const TrayMenuModel& model,
					 const TrayMenuInvoker& invoke,
					 const std::function<void()>& onMenuClosed)
{
	TrayNativeMenuState state;
	VGMToyboxTrayMenuTarget* target = [[VGMToyboxTrayMenuTarget alloc] init];
	target->state = &state;

	NSMenu* menu = [[NSMenu alloc] initWithTitle: @"VGMToybox"];
	[menu setAutoenablesItems: NO];

	MacMenuRenderer renderer (menu, target);
	renderTrayMenu (model, renderer);

	if ([menu numberOfItems] > 0)
	{
		JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
		[statusItem popUpStatusItemMenu: menu];
		JUCE_END_IGNORE_WARNINGS_GCC_LIKE
	}

	const NSInteger selected = state.selectedIndex;
	[menu release];
	[target release];

	// Clear the menu-bar highlight before running the action: Quit may destroy this component.
	if (onMenuClosed != nullptr)
	{
		onMenuClosed();
	}

	if (selected >= 0 && selected < static_cast<NSInteger> (renderer.ids.size()))
	{
		if (invoke != nullptr)
		{
			invoke (renderer.ids[static_cast<std::size_t> (selected)]);
		}
	}
}
} // namespace

void TrayIcon::showPlatformMenu (const TrayMenuModel& model, const TrayMenuInvoker& invoke)
{
	if (auto* statusItem = static_cast<NSStatusItem*> (getNativeHandle()))
	{
		// popUpStatusItemMenu: highlights the item for the duration of the menu. Clear it before
		// the selected action runs, because Quit may destroy this component.
		setHighlighted (true);
		showNativeMenu (statusItem, model, invoke, [this] { setHighlighted (false); });
		return;
	}

	// The status item should exist after show(), but a missing native handle must not leave the
	// user with a right-click that does nothing.
	showJuceMenu (model, invoke);
}

} // namespace toybox::standalone
