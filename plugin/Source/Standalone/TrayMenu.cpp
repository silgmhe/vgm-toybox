#include "TrayMenu.h"

#include <cmath>
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
	const auto loaded = engine.isLoaded();

	TrayMenuModel menu;

	menu.push_back ({ {}, TrayMenuIcon::none,
					  { makeEntry ("tray.show", "Show Window", TrayMenuIcon::showWindow,
								   false, true, showWindow) } });

	// The transport rows. Play and pause share one row, because the menu says what the click
	// will do; with no song there is nothing to do, so the row is greyed.
	std::vector<TrayMenuEntry> transport;
	transport.push_back (makeEntry ("tray.playPause", engine.isPlaying() ? "Pause" : "Play",
									engine.isPlaying() ? TrayMenuIcon::pause : TrayMenuIcon::play,
									false, loaded, [this]
									{
										if (engine.isPlaying())
										{
											engine.pause();
										}
										else
										{
											engine.play();
										}
									}));
	transport.back().separatorBefore = true;
	transport.push_back (makeEntry ("tray.stop", "Stop", TrayMenuIcon::stop,
									false, loaded, [this] { engine.stop(); }));
	menu.push_back ({ {}, TrayMenuIcon::none, std::move (transport) });

	// Loop count, not loop points: the points come from the song and are not editable anywhere.
	const auto loopCount = engine.getLoopCount();
	const int loopCounts[] = { 1, 2, 3, 0 };
	const char* loopLabels[] = { "1x", "2x", "3x", "Loop forever" };
	std::vector<TrayMenuEntry> loops;
	for (int i = 0; i < 4; ++i)
	{
		const auto count = loopCounts[i];
		const auto id = count == 0 ? juce::String ("tray.loop.forever")
								   : juce::String ("tray.loop.") + juce::String (count);

		// Submenu rows keep the system check mark; only the parent group carries a glyph.
		loops.push_back (makeEntry (id, loopLabels[i], TrayMenuIcon::none,
									loopCount == count, true,
									[this, count] { engine.setLoopCount (count); }));
	}
	menu.push_back ({ "Loop", TrayMenuIcon::loop, std::move (loops) });

	// Fade is the song's ending fade, the same five choices as the transport panel.
	const auto fadeSeconds = engine.getFadeSeconds();
	const double fades[] = { 0.0, 1.0, 3.0, 5.0, 10.0 };
	const char* fadeLabels[] = { "No fade", "1 s fade", "3 s fade", "5 s fade", "10 s fade" };

	std::vector<TrayMenuEntry> fadeEntries;
	for (int i = 0; i < 5; ++i)
	{
		const auto seconds = fades[i];
		const auto id = juce::String ("tray.fade.") + juce::String ((int) seconds);

		// Submenu rows keep the system check mark; only the parent group carries a glyph.
		fadeEntries.push_back (makeEntry (id, fadeLabels[i], TrayMenuIcon::none,
										  std::abs (fadeSeconds - seconds) < 0.001,
										  true, [this, seconds] { engine.setFadeSeconds (seconds); }));
	}
	menu.push_back ({ "Fade", TrayMenuIcon::fade, std::move (fadeEntries) });

	auto quitEntry = makeEntry ("tray.quit", "Quit", TrayMenuIcon::quit, false, true, quit);
	quitEntry.separatorBefore = true;
	menu.push_back ({ {}, TrayMenuIcon::none, { std::move (quitEntry) } });

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
