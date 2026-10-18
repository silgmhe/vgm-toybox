#import <Cocoa/Cocoa.h>

#include "TrayIcon.h"
#include "TrayMenuIcons.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace toybox::standalone
{
/** The action list is owned by showNativeMenu() for the duration of the modal menu. Menu items
	only record their tag here; the function runs the action after AppKit has dismissed the menu,
	so a Quit cannot tear down the icon while the menu is still tracking.
*/
struct TrayNativeMenuActions
{
	std::vector<std::function<void()>> entries;
	NSInteger selectedIndex = -1;
};
} // namespace toybox::standalone

@interface VGMToyboxTrayMenuTarget : NSObject
{
@public
	toybox::standalone::TrayNativeMenuActions* actions;
}

- (void)invoke:(NSMenuItem*)item;
@end

@implementation VGMToyboxTrayMenuTarget
- (void)invoke:(NSMenuItem*)item
{
	if (actions != nullptr)
	{
		actions->selectedIndex = item.tag;
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

void addEntries (NSMenu* menu,
				 const TrayMenuGroup& group,
				 VGMToyboxTrayMenuTarget* target,
				 TrayNativeMenuActions& actions)
{
	for (const auto& entry : group.entries)
	{
		if (entry.separatorBefore)
		{
			[menu addItem: [NSMenuItem separatorItem]];
		}

		NSMenuItem* item = [[NSMenuItem alloc] initWithTitle: toNSString (entry.label)
													  action: @selector(invoke:)
											   keyEquivalent: @""];
		item.target = target;
		item.tag = static_cast<NSInteger> (actions.entries.size());
		[item setEnabled: entry.enabled ? YES : NO];
		[item setState: entry.ticked ? NSControlStateValueOn : NSControlStateValueOff];
		setItemImage (item, entry.icon);
		[menu addItem: item];
		[item release];

		actions.entries.push_back (entry.enabled ? entry.action : std::function<void()>());
	}
}

void addGroup (NSMenu* menu,
			   const TrayMenuGroup& group,
			   VGMToyboxTrayMenuTarget* target,
			   TrayNativeMenuActions& actions,
			   bool& hasEntries)
{
	if (group.title.isEmpty())
	{
		addEntries (menu, group, target, actions);
		hasEntries = hasEntries || ! group.entries.empty();
		return;
	}

	NSMenu* submenu = [[NSMenu alloc] initWithTitle: toNSString (group.title)];
	[submenu setAutoenablesItems: NO];
	addEntries (submenu, group, target, actions);

	if (hasEntries)
	{
		[menu addItem: [NSMenuItem separatorItem]];
	}

	NSMenuItem* item = [[NSMenuItem alloc] initWithTitle: toNSString (group.title)
												  action: nil
										   keyEquivalent: @""];
	[item setSubmenu: submenu];
	setItemImage (item, group.icon);
	[menu addItem: item];
	[item release];
	[submenu release];
	hasEntries = true;
}

void showNativeMenu (NSStatusItem* statusItem,
					 const TrayMenuModel& model,
					 const std::function<void()>& onMenuClosed)
{
	TrayNativeMenuActions actions;
	VGMToyboxTrayMenuTarget* target = [[VGMToyboxTrayMenuTarget alloc] init];
	target->actions = &actions;

	NSMenu* menu = [[NSMenu alloc] initWithTitle: @"VGMToybox"];
	[menu setAutoenablesItems: NO];

	bool hasEntries = false;
	for (const auto& group : model)
	{
		addGroup (menu, group, target, actions, hasEntries);
	}

	if ([menu numberOfItems] > 0)
	{
		JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
		[statusItem popUpStatusItemMenu: menu];
		JUCE_END_IGNORE_WARNINGS_GCC_LIKE
	}

	const NSInteger selected = actions.selectedIndex;
	[menu release];
	[target release];

	// Clear the menu-bar highlight before running the action: Quit may destroy this component.
	if (onMenuClosed != nullptr)
	{
		onMenuClosed();
	}

	if (selected >= 0 && selected < static_cast<NSInteger> (actions.entries.size()))
	{
		if (auto action = actions.entries[static_cast<std::size_t> (selected)])
		{
			action();
		}
	}
}
} // namespace

void TrayIcon::showPlatformMenu (const TrayMenuModel& model)
{
	if (auto* statusItem = static_cast<NSStatusItem*> (getNativeHandle()))
	{
		// popUpStatusItemMenu: highlights the item for the duration of the menu. Clear it before
		// the selected action runs, because Quit may destroy this component.
		setHighlighted (true);
		showNativeMenu (statusItem, model, [this] { setHighlighted (false); });
		return;
	}

	// The status item should exist after show(), but a missing native handle must not leave the
	// user with a right-click that does nothing.
	showJuceMenu (model);
}

} // namespace toybox::standalone
