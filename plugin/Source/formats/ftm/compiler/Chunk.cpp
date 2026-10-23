#include "Chunk.h"

namespace toybox::formats::ftm::compiler
{

void Chunk::ChangeByte (int index, std::uint8_t value)
{
	if (index < 0 || index >= (int) items.size())
	{
		return;
	}

	if (auto* item = dynamic_cast<ChunkByte*> (items[(std::size_t) index].get()))
	{
		item->data = value;
	}
}

void Chunk::SetupBankData (int index, unsigned char value)
{
	if (index < 0 || index >= (int) items.size())
	{
		return;
	}

	if (auto* item = dynamic_cast<ChunkBankReference*> (items[(std::size_t) index].get()))
	{
		item->bank = value;
	}
}

const std::vector<std::uint8_t>& Chunk::GetStringData (int index) const
{
	static const std::vector<std::uint8_t> empty;

	if (index < 0 || index >= (int) items.size())
	{
		return empty;
	}

	if (const auto* item = dynamic_cast<const ChunkString*> (items[(std::size_t) index].get()))
	{
		return item->data;
	}

	return empty;
}

std::uint8_t Chunk::GetStringData (int index, int position) const
{
	const auto& bytes = GetStringData (index);
	return (position >= 0 && position < (int) bytes.size()) ? bytes[(std::size_t) position] : 0;
}

std::string Chunk::GetDataRefName (int index) const
{
	if (index < 0 || index >= (int) items.size())
	{
		return {};
	}

	if (const auto* item = dynamic_cast<const ChunkReference*> (items[(std::size_t) index].get()))
	{
		return item->name;
	}

	return {};
}

void Chunk::UpdateDataRefName (int index, const std::string& label)
{
	if (index < 0 || index >= (int) items.size())
	{
		return;
	}

	if (auto* item = dynamic_cast<ChunkReference*> (items[(std::size_t) index].get()))
	{
		item->name = label;
	}
}

} // namespace toybox::formats::ftm::compiler
