#include "playback/BackendRegistry.h"

#include "backends/libvgm/LibvgmBackend.h"
#include "backends/nes/NesBackend.h"

namespace toybox::playback
{

/**
	Fills the registry with the backends this build was compiled with.

	The product is a VST3 plugin, so every backend ships inside the same signed binary - there
	is deliberately no runtime loading (ADR-008 in docs/desgin/playback-backend-design.md). A backend
	that cannot be shipped unconditionally (license, size) registers under its own compile-time
	switch and simply is not there when the switch is off.
*/
void registerBuiltInBackends (void)
{
	static bool registered = false;
	if (registered)
	{
		return;
	}
	registered = true;

	// the first backend that recognises an image wins, so this order is the priority.
	//
	// Modules go first because they need the chip cores: libvgm can open a .ftm too, by compiling it
	// to a VGM image, but the VGM format has no command for VRC6, N163 or VRC7, so that path drops
	// those voices without a word. The NES backend plays them.
	BackendRegistry::getInstance().add (std::make_unique<backends::nes::NesBackend>());
	BackendRegistry::getInstance().add (std::make_unique<backends::libvgm::LibvgmBackend>());

	// optional backends go here, e.g. a SNES/SPC engine:
	// #if TOYBOX_ENABLE_SPC_BACKEND
	//	BackendRegistry::getInstance().add (std::make_unique<backends::spc::SpcBackend>());
	// #endif
}

} // namespace toybox::playback
