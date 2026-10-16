#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The window, as text.

	A console runner cannot look at the UI, so it reads this instead: one JSON object per component,
	nested by indentation, carrying the fields an assertion actually needs - the component's ID, its
	type, its bounds, whether it is visible and enabled, and its text or toggle state where it has
	one. That turns questions like "did the mixer card appear?", "which page is showing?", "is the
	loop button on?" into a line of text instead of a screenshot.

	Deliberately not a screenshot: the interesting failures in this project (a modal window that was
	never shown, a tick that never moved) are *structure*, not pixels - and structure survives
	headless, in CI, at any DPI. `vgm_ui_test` keeps the pixel invariants; this covers behaviour.

	The v1 walks the child components. JUCE's accessibility tree (Component::createAccessibilityHandler,
	AccessibilityHandler::getChildren) is the better source where it exists - it carries roles and
	titles for screen readers too - and the dumper will prefer it as the custom-painted controls grow
	handlers (see docs/desgin/testing-architecture.md §3.2).
*/
namespace snapshot
{

/** Minimal JSON string quoting - enough for paths and labels in a diagnostic dump. */
inline juce::String quoted (const juce::String& text)
{
	return "\"" + text.replace ("\\", "\\\\").replace ("\"", "\\\"")
					 .replace ("\n", "\\n").replace ("\r", "").replace ("\t", " ") + "\"";
}

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

/** One component as a JSON object; `checked` is only meaningful on a clicking toggle button.

	`visible` is the component's own flag, not `isShowing()`: a headless run has no peer, so
	`isShowing()` would be false for everything and say nothing. What an assertion wants is "is this
	card part of the page it belongs to", which is exactly the own flag. */
inline juce::String line (juce::Component& component)
{
	const auto bounds = component.getBounds();
	juce::String out;

	out << "{\"id\":" << quoted (component.getComponentID())
		<< ",\"type\":" << quoted (typeNameOf (component))
		<< ",\"bounds\":[" << bounds.getX() << "," << bounds.getY()
		<< "," << bounds.getWidth() << "," << bounds.getHeight() << "]"
		<< ",\"visible\":" << (component.isVisible() ? "true" : "false")
		<< ",\"enabled\":" << (component.isEnabled() ? "true" : "false");

	if (auto* button = dynamic_cast<juce::Button*> (&component);
		button != nullptr && button->getClickingTogglesState())
	{
		out << ",\"checked\":" << (button->getToggleState() ? "true" : "false");
	}

	out << ",\"text\":" << quoted (textOf (component)) << "}";

	return out;
}

inline void appendTree (juce::String& out, juce::Component& component, int depth, int maxDepth)
{
	if (maxDepth >= 0 && depth > maxDepth)
	{
		return;
	}

	out << juce::String::repeatedString ("  ", depth) << line (component) << juce::newLine;

	for (auto* child : component.getChildren())
	{
		if (child != nullptr)
		{
			appendTree (out, *child, depth + 1, maxDepth);
		}
	}
}

} // namespace snapshot

/** The whole tree under `root`, one JSON object per line, indented by depth. */
inline juce::String dumpComponentTree (juce::Component& root, int maxDepth = -1)
{
	juce::String out;
	snapshot::appendTree (out, root, 0, maxDepth);
	return out;
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

} // namespace toybox::ui
