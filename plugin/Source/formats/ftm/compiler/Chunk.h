#pragma once

// The compiler's output buffer, ported from FamiTracker's Chunk.h/Chunk.cpp (133 + 140 lines).
//
// A chunk is a list of data items rather than a byte vector, because the compiler emits things it
// does not know the value of yet: a word that points at a label defined in another chunk, or a byte
// that names the bank such a label lives in. Keeping the items symbolic until assembly is what lets
// the driver be laid out in any order.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace toybox::formats::ftm::compiler
{

enum class ChunkType
{
	header,
	sequence,
	instrumentList,
	instrument,
	sampleList,
	samplePointers,
	grooveList,
	groove,
	songList,
	song,
	frameList,
	frame,
	pattern,
	wavetable,
	waves,
	channelMap,
	channelTypes
};

/** One item in a chunk: a fixed value, a reference to a label, or a bank reference. */
class ChunkItem
{
public:
	virtual ~ChunkItem() = default;

	virtual int Size() const = 0;
	virtual std::uint16_t Data() const = 0;
};

class ChunkByte final : public ChunkItem
{
public:
	explicit ChunkByte (std::uint8_t value) : data (value) {}
	int Size() const override { return 1; }
	std::uint16_t Data() const override { return data; }

	std::uint8_t data = 0;
};

class ChunkWord final : public ChunkItem
{
public:
	explicit ChunkWord (std::uint16_t value) : data (value) {}
	int Size() const override { return 2; }
	std::uint16_t Data() const override { return data; }

	std::uint16_t data = 0;
};

class ChunkReference final : public ChunkItem
{
public:
	explicit ChunkReference (std::string label) : name (std::move (label)) {}
	int Size() const override { return 2; }
	std::uint16_t Data() const override { return address; }

	std::string name;
	std::uint16_t address = 0;			// filled in when the label is placed
};

class ChunkBankReference final : public ChunkItem
{
public:
	ChunkBankReference (std::string label, int value) : name (std::move (label)), bank (value) {}
	int Size() const override { return 1; }
	std::uint16_t Data() const override { return (std::uint16_t) bank; }

	std::string name;					// the label whose bank this byte should name
	unsigned int bank = 0;
};

class ChunkString final : public ChunkItem
{
public:
	explicit ChunkString (std::vector<std::uint8_t> bytes) : data (std::move (bytes)) {}
	int Size() const override { return (int) data.size(); }
	std::uint16_t Data() const override { return 0; }	// invalid for this type, as in the original

	std::vector<std::uint8_t> data;
};

class Chunk
{
public:
	Chunk (ChunkType type, std::string label) : kind (type), name (std::move (label)) {}

	void Clear() { items.clear(); }

	ChunkType GetType() const { return kind; }
	const std::string& GetLabel() const { return name; }

	void SetBank (unsigned char value) { bank = value; }
	unsigned char GetBank() const { return bank; }

	int GetLength() const { return (int) items.size(); }
	std::uint16_t GetData (int index) const { return items[(std::size_t) index]->Data(); }
	std::uint16_t GetDataSize (int index) const { return (std::uint16_t) items[(std::size_t) index]->Size(); }

	void StoreByte (std::uint8_t value) { items.push_back (std::make_unique<ChunkByte> (value)); }
	void StoreWord (std::uint16_t value) { items.push_back (std::make_unique<ChunkWord> (value)); }
	void StoreReference (std::string label)
	{
		items.push_back (std::make_unique<ChunkReference> (std::move (label)));
	}
	void StoreBankReference (std::string label, int value)
	{
		items.push_back (std::make_unique<ChunkBankReference> (std::move (label), value));
	}
	void StoreString (std::vector<std::uint8_t> bytes)
	{
		items.push_back (std::make_unique<ChunkString> (std::move (bytes)));
	}

	void ChangeByte (int index, std::uint8_t value);
	void SetupBankData (int index, unsigned char value);
	const std::vector<std::uint8_t>& GetStringData (int index) const;
	std::uint8_t GetStringData (int index, int position) const;

	/** The label a reference points at, or an empty string when the item is something else. */
	std::string GetDataRefName (int index) const;
	void UpdateDataRefName (int index, const std::string& label);

private:
	ChunkType kind;
	std::string name;
	unsigned char bank = 0;
	std::vector<std::unique_ptr<ChunkItem>> items;
};

} // namespace toybox::formats::ftm::compiler
