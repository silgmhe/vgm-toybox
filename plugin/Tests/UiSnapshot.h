#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::test
{

/**
	The window, as a JSON tree.

	A console runner cannot look at the UI, so it reads this instead: one object per component,
	nested under its parent's `children`, carrying the fields a reader actually needs - the
	component's ID, its type, its bounds, whether it is visible and enabled, and its text or toggle
	state where it has one. That turns questions like "did the mixer card appear?", "which page is
	showing?", "is the loop button on?" into something a program can parse and a person can scan.

	**A real tree, not an indented list.** This used to emit one JSON object per component, nested
	only by whitespace - readable, but not parseable as a whole: `dump`'s output could not be handed
	to any JSON reader. It is built as `var` objects now and formatted by `juce::JSON`, so the
	output is one valid document and the string escaping is JUCE's rather than ours.

	Deliberately not a screenshot: the interesting failures in this project (a modal window that was
	never shown, a tick that never moved) are *structure*, not pixels - and structure survives
	headless, in CI, at any DPI. `vgm_ui_test` keeps the pixel invariants; this covers behaviour.

	**This header lives in `Tests/`, not in `Source/ui/`.** Its only consumer is the harness; a
	diagnostic that no shipped code calls has no business in the product tree.
*/
namespace snapshot
{

/** "class toybox::ui::IconButton" -> "IconButton": the type as a human reads it. */
inline juce::String typeNameOf (juce::Component& component)
{
	auto name = juce::String (typeid (component).name());
	name = name.fromLastOccurrenceOf ("::", false, false);
	return name.startsWithIgnoreCase ("class ") ? name.substring (6) : name;
}

/** The text a human would read on this component, if it shows any. */
inline juce::String textOf (juce::Component& component)
{
	if (auto* label = dynamic_cast<juce::Label*> (&component))
	{
		return label->getText();
	}
	if (auto* button = dynamic_cast<juce::Button*> (&component))
	{
		return button->getButtonText();
	}
	if (auto* slider = dynamic_cast<juce::Slider*> (&component))
	{
		return slider->getTextFromValue (slider->getValue());
	}
	if (auto* box = dynamic_cast<juce::ComboBox*> (&component))
	{
		return box->getText();
	}
	return {};
}

/** The component and everything under it, as a `var` tree ready for `JSON::toString`.

	`visible` is the component's own flag, not `isShowing()`: a headless run has no peer, so
	`isShowing()` would be false for everything and say nothing. What an assertion wants is "is this
	card part of the page it belongs to", which is exactly the own flag.

	`children` is omitted when there are none, rather than written as an empty array: a leaf is the
	common case here, and the braces read better without a dangling `[]` on each one.

	`checked` is only meaningful on a clicking toggle button; `value` only on a slider (a seek bar
	shows its position only there, and `text` on a slider is the *formatted* value, which is not what
	an assertion wants).
*/
inline juce::var componentAsVar (juce::Component& component, int depth = 0, int maxDepth = -1)
{
	auto* object = new juce::DynamicObject();
	const juce::var result (object);

	const auto bounds = component.getBounds();
	juce::Array<juce::var> box;
	box.add (bounds.getX());
	box.add (bounds.getY());
	box.add (bounds.getWidth());
	box.add (bounds.getHeight());

	object->setProperty ("id", component.getComponentID());
	object->setProperty ("type", typeNameOf (component));
	object->setProperty ("bounds", box);
	object->setProperty ("visible", component.isVisible());
	object->setProperty ("enabled", component.isEnabled());

	if (auto* button = dynamic_cast<juce::Button*> (&component);
		button != nullptr && button->getClickingTogglesState())
	{
		object->setProperty ("checked", button->getToggleState());
	}

	if (auto* slider = dynamic_cast<juce::Slider*> (&component))
	{
		object->setProperty ("value", slider->getValue());
	}

	object->setProperty ("text", textOf (component));

	if (maxDepth >= 0 && depth >= maxDepth)
	{
		return result;
	}

	juce::Array<juce::var> children;

	for (auto* child : component.getChildren())
	{
		if (child != nullptr)
		{
			children.add (componentAsVar (*child, depth + 1, maxDepth));
		}
	}

	if (! children.isEmpty())
	{
		object->setProperty ("children", children);
	}

	return result;
}

/** The tree under `root`, as formatted JSON. `maxDepth` of 0 stops at `root` itself.

	The line endings are normalised to `\n` on the way out. `juce::newLine` is `\r\n` on Windows, and
	`printf` in text mode turns each `\n` into another `\r\n` - so the raw string came out with a blank
	line between every field, which doubled the height of every dump.
*/
inline juce::String dumpTree (juce::Component& root, int maxDepth = -1)
{
	return juce::JSON::toString (componentAsVar (root, 0, maxDepth), false).replace ("\r\n", "\n");
}

/** How many components the tree holds - a one-line answer to "was anything built at all?". */
inline int countComponents (juce::Component& root)
{
	int count = 1;

	for (auto* child : root.getChildren())
	{
		if (child != nullptr)
		{
			count += countComponents (*child);
		}
	}

	return count;
}

} // namespace snapshot

} // namespace toybox::test
