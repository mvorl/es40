#include "MPU401.h"
#include <vector>

#ifdef _WIN32
#include "AliM1543C.h"
#pragma comment(lib, "winmm.lib")

const CSystemComponent* CMPU401::memory_decode_owner() const noexcept
{
	return isa_bridge ? static_cast<const CSystemComponent*>(isa_bridge) : this;
}

bool CMPU401::decodes_memory_access(int index, u64 address, int, bool) const noexcept
{
	if (!((index == 0 || index == 1) && address == 0))
		return false;
	if (!isa_bridge)
		return true;
	const u32 port = 0x330u + (u32)index;
	return isa_bridge->isa_io_decode(port) != CAliM1543C::IsaIoDecode::None;
}

bool CMPU401::uses_subtractive_decode(int index, u64 address, int dsize,
	bool write) const noexcept
{
	if (!isa_bridge || !decodes_memory_access(index, address, dsize, write))
		return false;
	const u32 port = 0x330u + (u32)index;
	return isa_bridge->isa_io_decode(port) == CAliM1543C::IsaIoDecode::Subtractive;
}

u64 CMPU401::ReadMem(int index, u64 address, int dsize)
{
	//fprintf(stderr, "MPU401: ReadMem: index=%d, address=0x%llx, dsize=%d\n", index, address, dsize);
	if (dsize == 16 && !(address & 1))
	{
		u64 res = 0;
		res |= ReadMem(index, address, 8) << 0;
		res |= ReadMem(index, address + 1, 16) << 0;
		return res;
	}
	if (dsize > 16)
	{
		return 0;
	}
	if (address & 1)
	{
		midi_mpu_status |= 0x80;
		if (bReset) {
			bReset = false;
			return 0xfe;
		}
		return 0xfe; // Always return Active Sensing.
	}
	else
	{
		return midi_mpu_status;
	}
}

void CMPU401::WriteMem(int index, u64 address, int dsize, u64 data)
{
	//fprintf(stderr, "MPU401: WriteMem: index=%d, address=0x%llx, dsize=%d, data=0x%llX\n", index, address, dsize, data);
	if (dsize == 16 && !(address & 1))
	{
		WriteMem(index, address, 8, (data >> 0) & 0xff);
		WriteMem(index, address + 1, 8, (data >> 8) & 0xff);
		return;
	}
	if (dsize > 16)
	{
		return;
	}
	if (address & 1)
	{
		if (data == 0x3f)
		{
			midi_mpu_status &= ~0x80;
		}
		if (data == 0xff)
		{
			bReset = true;
			midi_ptr = 0;
			midi_mpu_status &= ~0x80;
		}
	}
	else
	{
		if (!bReset)
		{
			if (midi_status_byte == 0xF0 && data != 0xF7) 
			{
				sysex_data.push_back(data);
				return;
			}
			if (midi_status_byte == 0xF0 && data == 0xF7)
			{
				sysex_data.push_back(data);
				hdr.dwBufferLength = sysex_data.size();
				hdr.lpData = (LPSTR)sysex_data.data();
				midiOutPrepareHeader(hmo, &hdr, sizeof(hdr));
				midiOutLongMsg(hmo, &hdr, sizeof(hdr));
				midi_status_byte = 0x80;
				midi_buffer[0] = 0x80;
				midi_ptr = 0;
				return;
			}
			if ((data & 0xF0) < 0x80 && midi_ptr == 0)
			{
				midi_buffer[0] = midi_status_byte;
				midi_ptr = 1;
			}

			midi_buffer[midi_ptr++] = data;
			midi_status_byte = midi_buffer[0];

			if (midi_buffer[0] == 0xF0)
			{
				sysex_data.clear();
				sysex_data.push_back(0xF0);
				return;
			}

			if (midi_ptr >= midi_lengths[(midi_buffer[0] >> 4) - 0x8])
			{
				midiOutShortMsg(hmo, *(DWORD*)(&midi_buffer[0]));
				midi_ptr = 0;
			}
		}
	}
	return;
}

#endif
