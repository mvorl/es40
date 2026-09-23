#pragma once

#ifdef _WIN32
#include "StdAfx.h"
#include "System.h"
#include "SystemComponent.h"

#include <windows.h>
#include <mmsystem.h>

class CAliM1543C;

class CMPU401 : public CSystemComponent
{
  virtual int   RestoreState(FILE* f) override
  {
    return 0;
  }
  virtual int   SaveState(FILE* f) override
  {
    return 0;
  }

private:
  // Borrowed routing owner; not part of the saved device state.
  const CAliM1543C* isa_bridge = nullptr;

  HMIDIOUT hmo;
  MIDIHDR hdr;

  const int midi_lengths[8] = { 3, 3, 3, 3, 2, 2, 3, 1 };
  unsigned char midi_buffer[32] = {};
  unsigned char midi_ptr = 0;
  unsigned char midi_status_byte = 0x80;
  unsigned char midi_mpu_status = 0x80;
  bool bReset = false;
  std::vector<uint8_t> sysex_data;

public:
  CMPU401(CConfigurator* cfg, CSystem* c) : CSystemComponent(cfg, c)
  {
    MIDIOUTCAPSA caps;
    if (midiOutOpen(&hmo, cfg->get_num_value("midi_out", true, 0), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
      FAILURE(Configuration, "MPU-401: Failed to open MIDI output device.");
    }
    UINT id = 0;
    if (midiOutGetID(hmo, &id) == MMSYSERR_NOERROR)
    {
      midiOutGetDevCapsA(id, &caps, sizeof(caps));
      printf("MPU-401: Opened MIDI device \"%s\".\n", caps.szPname);
    }
    else
    {
      printf("MPU-401: Opened MIDI device.\n");
    }
    ZeroMemory(&hdr, sizeof(hdr));
    c->RegisterMemory(this, 0, U64(0x00000801fc000330), 1);
    c->RegisterMemory(this, 1, U64(0x00000801fc000331), 1);
  }

  virtual ~CMPU401()
  {
    midiOutClose(hmo);
  }
  void bind_isa_bridge(CAliM1543C* bridge) override { isa_bridge = bridge; }
  const CSystemComponent* memory_decode_owner() const noexcept override;
  bool decodes_memory_access(int index, u64 address, int dsize,
    bool write) const noexcept override;
  bool uses_subtractive_decode(int index, u64 address, int dsize,
    bool write) const noexcept override;

  virtual u64   ReadMem(int index, u64 address, int dsize) override;
  virtual void  WriteMem(int index, u64 address, int dsize, u64 data) override;
};

#endif
