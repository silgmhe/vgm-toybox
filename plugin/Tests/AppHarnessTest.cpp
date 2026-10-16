/*
	Console harness for the window - Phase 0 probe of docs/desgin/testing-architecture.md.

	The question it answers: can the editor be built, laid out and *read* without a window, a mouse
	or a screenshot? If this runs, the whole testability plan is feasible - the editor exists as a
	plain object, its structure comes out as text, and every later phase only adds commands and
	assertions to what is already here.

	It links the plugin's shared code (the STATIC target juce_add_plugin creates), so what it drives
	is the same ToyboxAudioProcessor and ToyboxAudioProcessorEditor the application uses - a harness
	that ran a copy of the UI would prove nothing.

	Not built by default:
	    cmake --build <build dir> --target vgm_app_test
	    <build dir>/plugin/vgm_app_test
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/UiSnapshot.h"

int main (void)
{
	// GUI objects without a window: this is what makes a console harness possible at all
	juce::ScopedJuceInitialiser_GUI juceInitialiser;

	// The harness speaks for the standalone build, because that is where the window is ours (Close
	// and Exit exist there, and a song can arrive on the command line). `wrapperType` is const by
	// design, so this is JUCE's own hook for exactly this situation - it applies to the next
	// processor constructed.
	juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);

	ToyboxAudioProcessor processor;

	ToyboxAudioProcessorEditor editor (processor);
	editor.setSize (820, 650);		// lays the tree out; never added to the desktop

	const int count = toybox::ui::countComponents (editor);
	const auto tree = toybox::ui::dumpComponentTree (editor);

	int withId = 0;
	{
		std::function<void (juce::Component&)> countIds = [&countIds, &withId] (juce::Component& c)
		{
			if (c.getComponentID().isNotEmpty())
			{
				++withId;
			}
			for (auto* child : c.getChildren())
			{
				if (child != nullptr)
				{
					countIds (*child);
				}
			}
		};
		countIds (editor);
	}

	std::printf ("=== can the editor exist without a window? ===\n");
	std::printf ("components: %d, with an ID: %d\n\n", count, withId);
	std::printf ("%s", tree.toRawUTF8());

	return count > 0 ? 0 : 1;
}
