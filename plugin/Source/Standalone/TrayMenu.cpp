#include "TrayMenu.h"

#include "i18n/LanguageManager.h"
#include "playback/TransportPresets.h"

#include <cstddef>
#include <utility>

namespace toybox::standalone
{

namespace
{
	TrayMenuEntry makeEntry (juce::String id, juce::String label, TrayMenuIcon icon,
							 bool ticked, bool enabled, std::function<void()> action)
	{
		TrayMenuEntry entry;
		entry.id = std::move (id);
		entry.label = std::move (label);
		entry.icon = icon;
		entry.ticked = ticked;
		entry.enabled = enabled;
		entry.action = std::move (action);
		return entry;
	}
}

TrayMenuGroup TrayMenuGroup::topLevel (std::vector<TrayMenuEntry> entries)
{
	TrayMenuGroup group;
	group.entries = std::move (entries);
	return group;
}

TrayMenuGroup TrayMenuGroup::submenu (juce::String title, TrayMenuIcon icon,
									  std::vector<TrayMenuEntry> entries)
{
	TrayMenuGroup group;
	group.title = std::move (title);
	group.icon = icon;
	group.entries = std::move (entries);
	return group;
}

void renderTrayMenu (const TrayMenuModel& model, TrayMenuRenderer& renderer)
{
	bool hasEntries = false;

	for (const auto& group : model)
	{
		if (group.isSubmenu())
		{
			if (hasEntries)
			{
				renderer.addSeparator();
			}

			renderer.beginSubmenu (group);
		}

		for (const auto& entry : group.entries)
		{
			if (entry.separatorBefore)
			{
				renderer.addSeparator();
			}

			renderer.addItem (entry);
			hasEntries = true;
		}

		if (group.isSubmenu())
		{
			renderer.endSubmenu();
		}
	}
}

TrayMenu::TrayMenu (PlaybackEngine& engineToUse,
					std::function<void()> showWindowCallback,
					std::function<void()> quitCallback)
	: engine (engineToUse),
	  showWindow (std::move (showWindowCallback)),
	  quit (std::move (quitCallback))
{
}

TrayMenuModel TrayMenu::build (void) const
{
	const bool loaded = engine.isLoaded();

	// Take one snapshot for the label, the glyph and the action. Reading the engine again inside
	// the action would let a song that ended while the menu was open turn a click on "Pause" into
	// a play.
	const bool playing = engine.isPlaying();

	TrayMenuModel menu;

	menu.push_back (TrayMenuGroup::topLevel (
		{ makeEntry ("tray.show", i18n::tr ("tray.show"), TrayMenuIcon::showWindow,
					 false, true, showWindow) }));

	// The transport rows. Play and pause share one row, because the menu says what the click
	// will do; with no song there is nothing to do, so the row is greyed. The id names the
	// action, not the widget: if playback changes while the menu is open, the stale id is no
	// longer in the rebuilt model and the click is refused rather than doing the opposite.
	std::vector<TrayMenuEntry> transport;
	transport.push_back (makeEntry (playing ? "tray.pause" : "tray.play",
									playing ? i18n::tr ("tray.pause") : i18n::tr ("tray.play"),
									playing ? TrayMenuIcon::pause : TrayMenuIcon::play,
									false, loaded, [this, playing]
									{
										if (playing)
										{
											engine.pause();
										}
										else
										{
											engine.play();
										}
									}));
	transport.back().separatorBefore = true;
	transport.push_back (makeEntry ("tray.stop", i18n::tr ("tray.stop"), TrayMenuIcon::stop,
									false, loaded, [this] { engine.stop(); }));
	menu.push_back (TrayMenuGroup::topLevel (std::move (transport)));

	// Loop count, not loop points: the points come from the song and are not editable anywhere.
	// The tick uses the same row-selection rule as the transport panel, so an out-of-range count
	// restored from an old session still marks one row instead of none.
	const int loopIndex = transport::loopPresetIndex (engine.getLoopCount());
	std::vector<TrayMenuEntry> loops;

	for (std::size_t i = 0; i < transport::loopPresets.size(); ++i)
	{
		const auto& preset = transport::loopPresets[i];
		const auto id = preset.count == 0
			? juce::String ("tray.loop.forever")
			: juce::String ("tray.loop.") + juce::String (preset.count);

		// Submenu rows keep the system check mark; only the parent group carries a glyph.
		loops.push_back (makeEntry (id, i18n::tr (preset.labelKey), TrayMenuIcon::none,
									loopIndex == (int) i, true,
									[this, count = preset.count] { engine.setLoopCount (count); }));
	}

	menu.push_back (TrayMenuGroup::submenu (i18n::tr ("tray.loop"), TrayMenuIcon::loop,
										   std::move (loops)));

	// Fade is the song's ending fade, the same five choices as the transport panel.
	const int fadeIndex = transport::fadePresetIndex (engine.getFadeSeconds());
	std::vector<TrayMenuEntry> fadeEntries;

	for (std::size_t i = 0; i < transport::fadePresets.size(); ++i)
	{
		const auto& preset = transport::fadePresets[i];
		const auto id = juce::String ("tray.fade.") + juce::String ((int) preset.seconds);

		// Submenu rows keep the system check mark; only the parent group carries a glyph.
		fadeEntries.push_back (makeEntry (id, i18n::tr (preset.labelKey), TrayMenuIcon::none,
										  fadeIndex == (int) i, true,
										  [this, seconds = preset.seconds]
										  { engine.setFadeSeconds (seconds); }));
	}

	menu.push_back (TrayMenuGroup::submenu (i18n::tr ("tray.fade"), TrayMenuIcon::fade,
										   std::move (fadeEntries)));

	auto quitEntry = makeEntry ("tray.quit", i18n::tr ("tray.quit"), TrayMenuIcon::quit,
								false, true, quit);
	quitEntry.separatorBefore = true;
	menu.push_back (TrayMenuGroup::topLevel ({ std::move (quitEntry) }));

	return menu;
}

bool TrayMenu::invoke (const juce::String& id) const
{
	for (const auto& group : build())
	{
		for (const auto& entry : group.entries)
		{
			if (entry.id != id)
			{
				continue;
			}

			if (! entry.enabled || entry.action == nullptr)
			{
				return false;
			}

			entry.action();
			return true;
		}
	}

	return false;
}

} // namespace toybox::standalone
