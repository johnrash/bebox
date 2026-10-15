/*
 * BeBox emulator - NCR 53C810 PCI SCSI I/O processor.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Ncr53c810.h"

#include "../Log.h"
#include "../bus/Mpc105.h"

#include <algorithm>
#include <cstdio>

namespace bebox {

enum {
	SCNTL0 = 0x00, SCNTL1 = 0x01, SCNTL2 = 0x02, SCNTL3 = 0x03, SCID = 0x04,
	SXFER = 0x05, SDID = 0x06, GPREG = 0x07, SFBR = 0x08, SOCL = 0x09,
	SSID = 0x0a, SBCL = 0x0b, DSTAT = 0x0c, SSTAT0 = 0x0d, SSTAT1 = 0x0e,
	SSTAT2 = 0x0f, DSA = 0x10, ISTAT = 0x14, CTEST0 = 0x18, CTEST1 = 0x19,
	CTEST2 = 0x1a, CTEST3 = 0x1b, TEMP = 0x1c, DFIFO = 0x20, CTEST4 = 0x21,
	CTEST5 = 0x22, CTEST6 = 0x23, DBC = 0x24, DCMD = 0x27, DNAD = 0x28,
	DSP = 0x2c, DSPS = 0x30, SCRATCHA = 0x34, DMODE = 0x38, DIEN = 0x39,
	SBR = 0x3a, DCNTL = 0x3b, ADDER = 0x3c, SIEN0 = 0x40, SIEN1 = 0x41,
	SIST0 = 0x42, SIST1 = 0x43, SLPAR = 0x44, MACNTL = 0x46, GPCNTL = 0x47,
	STIME0 = 0x48, STIME1 = 0x49, RESPID = 0x4a, STEST0 = 0x4c,
	STEST1 = 0x4d, STEST2 = 0x4e, STEST3 = 0x4f, SIDL = 0x50, SODL = 0x54,
	SBDL = 0x58, SCRATCHB = 0x5c
};

enum {
	ISTAT_DIP = 0x01, ISTAT_SIP = 0x02, ISTAT_INTF = 0x04, ISTAT_CON = 0x08,
	ISTAT_SEM = 0x10, ISTAT_SIGP = 0x20, ISTAT_SRST = 0x40,
	ISTAT_ABRT = 0x80,
	DSTAT_IID = 0x01, DSTAT_SIR = 0x04, DSTAT_SSI = 0x08, DSTAT_ABRT = 0x10,
	DSTAT_BF = 0x20, DSTAT_DFE = 0x80,
	SIST0_PAR = 0x01, SIST0_RST = 0x02, SIST0_UDC = 0x04, SIST0_SGE = 0x08,
	SIST0_RSL = 0x10, SIST0_SEL = 0x20, SIST0_CMP = 0x40, SIST0_MA = 0x80,
	SIST1_HTH = 0x01, SIST1_GEN = 0x02, SIST1_STO = 0x04
};

// Fatal SCSI interrupts halt the SCRIPTS processor.
static const u8 kFatalSist0 = SIST0_MA | SIST0_SGE | SIST0_UDC | SIST0_RST
	| SIST0_PAR;
static const u8 kFatalSist1 = SIST1_STO;


class Ncr53c810::IoPorts : public ByteIoDevice {
public:
	IoPorts(Ncr53c810& chip) : fChip(chip), fBase(0) {}
	void SetBase(u32 base) { fBase = base; }
	virtual u8 IoRead8(u32 port) { return fChip.ReadRegister(port - fBase); }
	virtual void IoWrite8(u32 port, u8 value)
		{ fChip.WriteRegister(port - fBase, value); }
	virtual const char* IoName() const { return "ncr53c810"; }

private:
	Ncr53c810& fChip;
	u32 fBase;
};


// Memory mapped registers: little endian device, lanes swapped for the
// big endian processor.
class Ncr53c810::Registers : public MmioDevice {
public:
	Registers(Ncr53c810& chip) : fChip(chip), fBase(0) {}
	void SetBase(u32 base) { fBase = base; }

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 offset = (addr - fBase) & 0xff;
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fChip.ReadRegister(offset + i);
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 offset = (addr - fBase) & 0xff;
		for (int i = 0; i < size; i++)
			fChip.WriteRegister(offset + i, (u8)(value >> (8 * (size - 1 - i))));
	}

	virtual const char* MmioName() const { return "ncr53c810-mmio"; }

private:
	Ncr53c810& fChip;
	u32 fBase;
};


Ncr53c810::Ncr53c810(PhysBus& bus, IoSpace& io, Mpc105& bridge,
	Scheduler& scheduler, u64 ticksPerSecond)
	:
	PciDevice(0x1000, 0x0001, 0x010000, 0x02),
	fBus(bus),
	fIo(io),
	fBridge(bridge),
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fIoBase(0),
	fMemoryBase(0),
	fIrqLevel(false)
{
	DeclareBar(0, kBarIo, 256);
	DeclareBar(1, kBarMemory, 256);
	SetInterruptPin(1);
	fIoPorts = new IoPorts(*this);
	fMmio = new Registers(*this);
	fTimer.reset(new Timer(scheduler, [this]() { _Run(); }, "ncr53c810"));
	fSelectionTimer.reset(new Timer(scheduler, [this]() {
		BB_LOG(kLogScsi, kLogTrace, "selection timeout (wait %d)", fWait);
		if (fWait != kWaitSelection)
			return;
		fWait = kNotWaiting;
		_ScsiInterrupt(0, SIST1_STO);
	}, "ncr53c810-selection"));
	Reset();
}


Ncr53c810::~Ncr53c810()
{
	if (fIoBase != 0)
		fIo.Unmap(fIoBase, 256);
	if (fMemoryBase != 0)
		fBus.Unmap(Mpc105::kPciMemoryBase + fMemoryBase, 0x1000);
	delete fIoPorts;
	delete fMmio;
}


void
Ncr53c810::AttachDevice(int id, std::unique_ptr<ScsiDevice> device)
{
	fDevices[id] = std::move(device);
}


void
Ncr53c810::Reset()
{
	memset(fRegs, 0, sizeof(fRegs));
	fRegs[SCNTL0] = 0xc0;
	fRegs[CTEST1] = 0xf0;
	fRegs[CTEST3] = 0x10;		// chip revision 1
	fRegs[GPCNTL] = 0x0f;
	fRegs[GPREG] = 0x0f;
	fRegs[SCID] = 0x07;
	fDstat = 0;
	fSist0 = fSist1 = 0;
	fCarry = false;
	fRunning = false;
	fWait = kNotWaiting;
	fTimer->Cancel();
	fSelectionTimer->Cancel();
	_BusReset();
	_UpdateIrq();
}


void
Ncr53c810::BarsChanged()
{
	if (fIoBase != 0) {
		fIo.Unmap(fIoBase, 256);
		fIoBase = 0;
	}
	if (fMemoryBase != 0) {
		fBus.Unmap(Mpc105::kPciMemoryBase + fMemoryBase, 0x1000);
		fMemoryBase = 0;
	}
	u32 io = BarAddress(0);
	if (IoEnabled() && io != 0) {
		fIoBase = io;
		fIoPorts->SetBase(io);
		fIo.Map(io, 256, fIoPorts);
	}
	u32 memory = BarAddress(1);
	if (MemoryEnabled() && memory != 0 && memory < 0x3f000000) {
		fMemoryBase = memory;
		// registers are mirrored over the page
		fMmio->SetBase(Mpc105::kPciMemoryBase + memory);
		fBus.MapDevice(Mpc105::kPciMemoryBase + (memory & ~0xfffu), 0x1000,
			fMmio);
	}
	BB_LOG(kLogScsi, kLogDebug, "ncr53c810: I/O %x, memory %x", fIoBase,
		fMemoryBase);
}


u32
Ncr53c810::_Reg32(u32 offset) const
{
	return LoadLE32(&fRegs[offset]);
}


void
Ncr53c810::_SetReg32(u32 offset, u32 value)
{
	StoreLE32(&fRegs[offset], value);
}


// #pragma mark - interrupts


void
Ncr53c810::_UpdateIrq()
{
	bool dma = (fDstat & fRegs[DIEN] & 0x7f) != 0;
	bool scsi = (fSist0 & fRegs[SIEN0]) != 0 || (fSist1 & fRegs[SIEN1]) != 0;
	bool fly = (fRegs[ISTAT] & ISTAT_INTF) != 0;
	bool level = (dma || scsi || fly) && (fRegs[DCNTL] & 0x02) == 0;
	if (level != fIrqLevel) {
		fIrqLevel = level;
		if (fIrq)
			fIrq(level);
	}
}


void
Ncr53c810::_DmaInterrupt(u8 dstat)
{
	fDstat |= dstat;
	fRunning = false;		// DMA interrupts always halt SCRIPTS
	fWait = kNotWaiting;
	BB_LOG(kLogScsi, kLogDebug, "ncr53c810: DMA interrupt %02x, dsp %08x "
		"dsps %08x", dstat, _Reg32(DSP), _Reg32(DSPS));
	_UpdateIrq();
}


void
Ncr53c810::_ScsiInterrupt(u8 sist0, u8 sist1)
{
	fSist0 |= sist0;
	fSist1 |= sist1;
	if ((sist0 & kFatalSist0) != 0 || (sist1 & kFatalSist1) != 0) {
		fRunning = false;
		fWait = kNotWaiting;
	}
	BB_LOG(kLogScsi, kLogDebug, "ncr53c810: SCSI interrupt %02x %02x, "
		"dsp %08x", sist0, sist1, _Reg32(DSP));
	_UpdateIrq();
}


// #pragma mark - registers


u8
Ncr53c810::ReadRegister(u32 offset)
{
	offset &= 0x7f;
	switch (offset) {
		case DSTAT:
		{
			u8 value = fDstat | DSTAT_DFE;
			fDstat = 0;
			_UpdateIrq();
			return value;
		}
		case SIST0:
		{
			u8 value = fSist0;
			fSist0 = 0;
			_UpdateIrq();
			return value;
		}
		case SIST1:
		{
			u8 value = fSist1;
			fSist1 = 0;
			_UpdateIrq();
			return value;
		}
		case ISTAT:
		{
			u8 value = fRegs[ISTAT] & (ISTAT_ABRT | ISTAT_SRST | ISTAT_SIGP
				| ISTAT_SEM | ISTAT_INTF);
			if (fTarget >= 0)
				value |= ISTAT_CON;
			if (fDstat != 0)
				value |= ISTAT_DIP;
			if (fSist0 != 0 || fSist1 != 0)
				value |= ISTAT_SIP;
			return value;
		}
		case CTEST2:
		{
			// Reading CTEST2 clears SIGP; bit 6 reports it.
			u8 value = (fRegs[ISTAT] & ISTAT_SIGP) != 0 ? 0x40 : 0;
			fRegs[ISTAT] &= ~ISTAT_SIGP;
			return value | 0x08;		// registers reachable as memory
		}
		case SBCL:
		{
			if (fTarget < 0)
				return 0;
			u8 value = 0x20 | (fPhase & 7);		// BSY, phase lines
			if (fPhase != kPhaseBusFree)
				value |= 0x80;					// REQ
			if (fAckHeld)
				value |= 0x40;
			if (fAttention)
				value |= 0x08;
			return value;
		}
		case SSTAT0:
			return fTarget >= 0 ? 0x04 : 0x00;	// won arbitration
		case SSTAT1:
			return fTarget >= 0 && fPhase != kPhaseBusFree ? (fPhase & 7) : 0;
		case SSTAT2:
			return fTarget < 0 ? 0x02 : 0x00;	// last disconnect
		case CTEST1:
			return 0xf0;						// DMA FIFO empty
		case DFIFO:
			return 0;
	}
	return fRegs[offset];
}


void
Ncr53c810::WriteRegister(u32 offset, u8 value)
{
	offset &= 0x7f;
	BB_LOG(kLogScsi, kLogTrace, "write %02x to register %02x (wait %d, "
		"running %d)", value, offset, fWait, fRunning);
	switch (offset) {
		case ISTAT:
		{
			if ((value & ISTAT_SRST) != 0) {
				Reset();
				fRegs[ISTAT] = ISTAT_SRST;
				return;
			}
			u8 old = fRegs[ISTAT];
			fRegs[ISTAT] = (old & ISTAT_INTF & ~(value & ISTAT_INTF))
				| (value & (ISTAT_SEM | ISTAT_SIGP));
			if ((value & ISTAT_ABRT) != 0 && fRunning) {
				fRunning = false;
				_DmaInterrupt(DSTAT_ABRT);
			}
			if ((value & ISTAT_SIGP) != 0 && fWait == kWaitReselect) {
				// abort WAIT RESELECT: continue at the alternate address
				fWait = kNotWaiting;
				_SetReg32(DSP, _Reg32(DNAD));
				fTimer->ArmIn(fTicksPerSecond / 1000000);
			}
			_UpdateIrq();
			return;
		}
		case DSTAT: case SIST0: case SIST1: case SSTAT0: case SSTAT1:
		case SSTAT2: case SBCL: case CTEST1: case CTEST3:
			return;		// read only
		case SCNTL1:
		{
			bool reset = (value & 0x08) != 0 && (fRegs[SCNTL1] & 0x08) == 0;
			fRegs[SCNTL1] = value;
			if (reset) {
				BB_LOG(kLogScsi, kLogInfo, "SCSI bus reset");
				_BusReset();
				for (auto& device : fDevices) {
					if (device)
						device->Reset();
				}
				_ScsiInterrupt(SIST0_RST, 0);
			}
			return;
		}
		case DSP + 3:
			fRegs[offset] = value;
			if ((fRegs[DMODE] & 0x01) == 0)
				_Start();
			return;
		case DCNTL:
			fRegs[DCNTL] = value & ~0x04;
			if ((value & 0x04) != 0)
				_Start();
			_UpdateIrq();
			return;
		case DIEN: case SIEN0: case SIEN1:
			fRegs[offset] = value;
			_UpdateIrq();
			return;
	}
	fRegs[offset] = value;
}


// #pragma mark - SCSI bus


void
Ncr53c810::_BusReset()
{
	fTarget = -1;
	fPhase = kPhaseBusFree;
	fAttention = false;
	fAckHeld = false;
	fMessageOut.clear();
	fMessageIn.clear();
	fMessageInPosition = 0;
	fDisconnectAfterMessage = false;
	fCommandReceived = false;
	fCdbExpected = 0;
	fDataPosition = 0;
	fDataOut.clear();
}


bool
Ncr53c810::_Select(int id, bool attention)
{
	if (id == (fRegs[SCID] & 7) || !fDevices[id])
		return false;
	_BusReset();
	fTarget = id;
	fAttention = attention;
	fCommand = ScsiCommand();
	fPhase = attention ? kPhaseMessageOut : kPhaseCommand;
	fRegs[SDID] = id;
	return true;
}


void
Ncr53c810::_Disconnect()
{
	fTarget = -1;
	fPhase = kPhaseBusFree;
	fAckHeld = false;
	fAttention = false;
}


// After the command: data, status, message in.
void
Ncr53c810::_TargetNextPhase()
{
	switch (fPhase) {
		case kPhaseCommand:
			if (!fCommand.dataIn.empty()) {
				fPhase = kPhaseDataIn;
				fDataPosition = 0;
			} else if (fCommand.dataOutLength > 0) {
				fPhase = kPhaseDataOut;
				fDataOut.clear();
			} else
				fPhase = kPhaseStatus;
			break;
		case kPhaseDataIn:
			fPhase = kPhaseStatus;
			break;
		case kPhaseDataOut:
			fDevices[fTarget]->DataOut(fCommand, fDataOut);
			fPhase = kPhaseStatus;
			break;
		case kPhaseStatus:
			fMessageIn = { 0x00 };		// COMMAND COMPLETE
			fMessageInPosition = 0;
			fDisconnectAfterMessage = true;
			fPhase = kPhaseMessageIn;
			break;
		default:
			break;
	}
}


void
Ncr53c810::_TargetReceive(u8 byte)
{
	switch (fPhase) {
		case kPhaseMessageOut:
		{
			fMessageOut.push_back(byte);
			// Is the message complete?
			u8 first = fMessageOut[0];
			bool complete = false;
			if (first == 0x01) {
				complete = fMessageOut.size() >= 2
					&& fMessageOut.size() >= (size_t)fMessageOut[1] + 2;
			} else if (first >= 0x20 && first <= 0x2f) {
				complete = fMessageOut.size() >= 2;
			} else
				complete = true;
			if (!complete)
				return;
			std::vector<u8> message;
			message.swap(fMessageOut);
			if ((first & 0x80) != 0) {
				fCommand.lun = first & 7;		// IDENTIFY
			} else if (first == 0x01 && message.size() >= 3
				&& message[2] == 0x01) {
				// SDTR: accept the requested period and offset
				fMessageIn = message;
				fMessageInPosition = 0;
				fDisconnectAfterMessage = false;
			} else if (first == 0x01) {
				fMessageIn = { 0x07 };			// MESSAGE REJECT
				fMessageInPosition = 0;
				fDisconnectAfterMessage = false;
			} else if (first == 0x06 || first == 0x0c) {
				// ABORT / BUS DEVICE RESET
				if (first == 0x0c)
					fDevices[fTarget]->Reset();
				_Disconnect();
				return;
			}
			if (!fAttention) {
				if (fMessageInPosition < fMessageIn.size())
					fPhase = kPhaseMessageIn;
				else
					fPhase = fCommandReceived ? kPhaseStatus : kPhaseCommand;
			}
			break;
		}
		case kPhaseCommand:
			if (fCommand.cdbLength < 16)
				fCommand.cdb[fCommand.cdbLength++] = byte;
			if (fCommand.cdbLength == 1) {
				switch (byte >> 5) {
					case 0: fCdbExpected = 6; break;
					case 1: case 2: fCdbExpected = 10; break;
					case 4: fCdbExpected = 16; break;
					case 5: fCdbExpected = 12; break;
					default: fCdbExpected = 6; break;
				}
			}
			if ((u32)fCommand.cdbLength >= fCdbExpected) {
				fCommandReceived = true;
				fDevices[fTarget]->Execute(fCommand);
				if (Log::Enabled(kLogScsi, kLogDebug)) {
					char cdb[16 * 3 + 1] = "";
					for (int i = 0; i < fCommand.cdbLength && i < 16; i++)
						sprintf(cdb + i * 3, "%02x ", fCommand.cdb[i]);
					BB_LOG(kLogScsi, kLogDebug, "target %d lun %d: %s-> status "
						"%02x, %zu bytes in, %u out", fTarget, fCommand.lun, cdb,
						fCommand.status, fCommand.dataIn.size(),
						fCommand.dataOutLength);
				}
				_TargetNextPhase();
			}
			break;
		case kPhaseDataOut:
			fDataOut.push_back(byte);
			if (fDataOut.size() >= fCommand.dataOutLength)
				_TargetNextPhase();
			break;
		default:
			break;
	}
}


bool
Ncr53c810::_TargetSend(u8& byte)
{
	switch (fPhase) {
		case kPhaseDataIn:
			if (fDataPosition >= fCommand.dataIn.size())
				return false;
			byte = fCommand.dataIn[fDataPosition++];
			if (fDataPosition >= fCommand.dataIn.size())
				_TargetNextPhase();
			return true;
		case kPhaseStatus:
			byte = fCommand.status;
			_TargetNextPhase();
			return true;
		case kPhaseMessageIn:
			if (fMessageInPosition >= fMessageIn.size())
				return false;
			byte = fMessageIn[fMessageInPosition++];
			return true;
		default:
			return false;
	}
}


// The initiator released ACK after a message in byte.
void
Ncr53c810::_TargetAckReleased()
{
	fAckHeld = false;
	if (fPhase != kPhaseMessageIn)
		return;
	if (fAttention) {
		fPhase = kPhaseMessageOut;
		return;
	}
	if (fMessageInPosition < fMessageIn.size())
		return;		// more message bytes
	if (fDisconnectAfterMessage) {
		_Disconnect();
		return;
	}
	fPhase = fCommandReceived ? kPhaseStatus : kPhaseCommand;
}


// #pragma mark - SCRIPTS


void
Ncr53c810::_Start()
{
	fRunning = true;
	fWait = kNotWaiting;
	fTimer->ArmIn(fTicksPerSecond / 1000000);
}


u32
Ncr53c810::_Fetch32(u32 address)
{
	u8 bytes[4];
	_Read(address, bytes, 4);
	return LoadLE32(bytes);
}


// SCRIPTS memory moves reach the chip's own registers internally. The
// BeOS driver (and the boot ROM) address them by the I/O base, so both
// bases are recognized.
bool
Ncr53c810::_InOurRegisters(u32 address) const
{
	return (fMemoryBase != 0 && address >= fMemoryBase
			&& address < fMemoryBase + 0x80)
		|| (fIoBase != 0 && address >= fIoBase && address < fIoBase + 0x80);
}


u32
Ncr53c810::_RegisterOffset(u32 address) const
{
	if (fMemoryBase != 0 && address >= fMemoryBase
		&& address < fMemoryBase + 0x80)
		return address - fMemoryBase;
	return address - fIoBase;
}


void
Ncr53c810::_Read(u32 address, void* buffer, u32 length)
{
	if (_InOurRegisters(address)) {
		u8* out = (u8*)buffer;
		for (u32 i = 0; i < length; i++)
			out[i] = ReadRegister(_RegisterOffset(address) + i);
		return;
	}
	fBridge.MasterRead(address, buffer, length);
}


void
Ncr53c810::_Write(u32 address, const void* buffer, u32 length)
{
	if (_InOurRegisters(address)) {
		const u8* in = (const u8*)buffer;
		for (u32 i = 0; i < length; i++)
			WriteRegister(_RegisterOffset(address) + i, in[i]);
		return;
	}
	fBridge.MasterWrite(address, buffer, length);
}


void
Ncr53c810::_Run()
{
	if (!fRunning || fWait != kNotWaiting)
		return;
	u64 ticks = 0;
	for (int i = 0; i < 64 && fRunning && fWait == kNotWaiting; i++) {
		if (!_Step())
			break;
		ticks += fTicksPerSecond / 5000000;		// 200 ns per instruction
		if ((fRegs[DCNTL] & 0x10) != 0 && fRunning) {
			_DmaInterrupt(DSTAT_SSI);			// single step
			break;
		}
	}
	if (fRunning && fWait == kNotWaiting)
		fTimer->ArmIn(std::max<u64>(ticks, fTicksPerSecond / 1000000));
}


bool
Ncr53c810::_Step()
{
	u32 dsp = _Reg32(DSP);
	u32 first = _Fetch32(dsp);
	u32 second = _Fetch32(dsp + 4);
	dsp += 8;
	_SetReg32(DSP, dsp);
	fRegs[DCMD] = first >> 24;
	fRegs[DBC] = (u8)first;
	fRegs[DBC + 1] = (u8)(first >> 8);
	fRegs[DBC + 2] = (u8)(first >> 16);
	_SetReg32(DSPS, second);
	BB_LOG(kLogScsi, kLogTrace, "scripts %08x: %08x %08x", dsp - 8, first,
		second);

	switch (first >> 30) {
		case 0:
			return _BlockMove(first >> 24, first & 0xffffff, second);
		case 1:
			if (((first >> 27) & 7) >= 5)
				return _ReadWrite(first);
			return _IoInstruction(first, second);
		case 2:
			return _TransferControl(first, second);
		default:
		{
			u32 destination = _Fetch32(dsp);
			_SetReg32(DSP, dsp + 4);
			_SetReg32(TEMP, destination);
			return _MemoryMove(first, second, destination);
		}
	}
}


bool
Ncr53c810::_BlockMove(u32 dcmd, u32 count, u32 address)
{
	int phase = dcmd & 7;
	if ((dcmd & 0x10) != 0) {
		// table indirect: { count, address } at DSA + signed offset
		s32 offset = ((s32)(address << 8)) >> 8;
		u32 entry = _Reg32(DSA) + offset;
		count = _Fetch32(entry) & 0xffffff;
		address = _Fetch32(entry + 4);
	} else if ((dcmd & 0x20) != 0) {
		address = _Fetch32(address);
	}
	_SetReg32(DNAD, address);

	if ((fRegs[SCNTL0] & 1) != 0) {
		BB_LOG(kLogScsi, kLogInfo, "target mode is not supported");
		_DmaInterrupt(DSTAT_IID);
		return false;
	}
	if (fTarget < 0) {
		// nobody to talk to: the SCSI bus is free
		_ScsiInterrupt(SIST0_UDC, 0);
		return false;
	}
	if (fAckHeld)
		_TargetAckReleased();
	auto setDbc = [this](u32 value) {
		fRegs[DBC] = (u8)value;
		fRegs[DBC + 1] = (u8)(value >> 8);
		fRegs[DBC + 2] = (u8)(value >> 16);
	};
	setDbc(count);
	if ((int)fPhase != phase) {
		_ScsiInterrupt(SIST0_MA, 0);
		return false;
	}

	bool sending = phase == kPhaseDataOut || phase == kPhaseCommand
		|| phase == kPhaseMessageOut;
	bool first = true;
	while (count > 0 && fTarget >= 0 && (int)fPhase == phase) {
		if (sending) {
			if (phase == kPhaseDataOut) {
				u32 chunk = std::min<u32>(count,
					fCommand.dataOutLength - (u32)fDataOut.size());
				size_t old = fDataOut.size();
				fDataOut.resize(old + chunk);
				_Read(address, &fDataOut[old], chunk);
				address += chunk;
				count -= chunk;
				if (fDataOut.size() >= fCommand.dataOutLength)
					_TargetNextPhase();
				continue;
			}
			u8 byte;
			_Read(address, &byte, 1);
			if (phase == kPhaseCommand && first) {
				// The CDB length comes from its group code.
				u32 length;
				switch (byte >> 5) {
					case 0: length = 6; break;
					case 1: case 2: length = 10; break;
					case 5: length = 12; break;
					default: length = count; break;
				}
				count = length;
			}
			// ATN goes away before the last message out byte.
			if (phase == kPhaseMessageOut && count == 1)
				fAttention = false;
			_TargetReceive(byte);
			address++;
			count--;
		} else {
			if (phase == kPhaseDataIn) {
				u32 available = (u32)(fCommand.dataIn.size() - fDataPosition);
				u32 chunk = std::min(count, available);
				if (chunk == 0)
					break;
				if (first)
					fRegs[SFBR] = fCommand.dataIn[fDataPosition];
				_Write(address, &fCommand.dataIn[fDataPosition], chunk);
				fDataPosition += chunk;
				address += chunk;
				count -= chunk;
				if (fDataPosition >= fCommand.dataIn.size())
					_TargetNextPhase();
				first = false;
				continue;
			}
			u8 byte;
			if (!_TargetSend(byte))
				break;
			if (first)
				fRegs[SFBR] = byte;
			_Write(address, &byte, 1);
			address++;
			count--;
			if (phase == kPhaseMessageIn) {
				fAckHeld = true;	// until CLEAR ACK or the next move
				if (count > 0)
					_TargetAckReleased();
			}
		}
		first = false;
	}
	setDbc(count);
	_SetReg32(DNAD, address);
	if (count > 0) {
		// the target changed phase (or disconnected) early
		if (fTarget < 0)
			_ScsiInterrupt(SIST0_UDC, 0);
		else
			_ScsiInterrupt(SIST0_MA, 0);
		return false;
	}
	return true;
}


bool
Ncr53c810::_IoInstruction(u32 first, u32 second)
{
	int opcode = (first >> 27) & 7;
	bool relative = (first & (1u << 26)) != 0;
	bool table = (first & (1u << 25)) != 0;
	bool attention = (first & (1u << 24)) != 0;
	int id = (first >> 16) & 7;
	u32 dsp = _Reg32(DSP);
	u32 alternate = relative ? dsp + (((s32)(second << 8)) >> 8) : second;
	_SetReg32(DNAD, alternate);

	if (table) {
		s32 offset = ((s32)(first << 8)) >> 8;
		u32 entry = _Fetch32(_Reg32(DSA) + offset);
		id = (entry >> 16) & 7;
		fRegs[SXFER] = (u8)(entry >> 8);
		fRegs[SCNTL3] = (u8)(entry >> 24);
	}
	if ((fRegs[SCNTL0] & 1) != 0) {
		BB_LOG(kLogScsi, kLogInfo, "target mode is not supported");
		_DmaInterrupt(DSTAT_IID);
		return false;
	}
	if (attention && opcode != 0) {
		_DmaInterrupt(DSTAT_IID);
		return false;
	}

	switch (opcode) {
		case 0:		// SELECT
			if (fTarget >= 0) {
				// already connected: the select is skipped (reselected)
				_SetReg32(DSP, alternate);
				return true;
			}
			if (_Select(id, attention)) {
				BB_LOG(kLogScsi, kLogDebug, "selected target %d", id);
				return true;
			}
			{
				// selection timeout
				int sel = fRegs[STIME0] & 0x0f;
				BB_LOG(kLogScsi, kLogDebug, "no target %d (timeout %d)", id,
					sel);
				if (sel == 0) {
					fWait = kWaitSelection;		// never times out
					return false;
				}
				u64 us = (u64)125 << (sel - 1);
				fWait = kWaitSelection;
				fSelectionTimer->ArmIn(us * fTicksPerSecond / 1000000);
				return false;
			}
		case 1:		// WAIT DISCONNECT
			if (fTarget >= 0 && !fAckHeld && fPhase == kPhaseBusFree)
				_Disconnect();
			else if (fTarget >= 0 && fPhase == kPhaseMessageIn
				&& fDisconnectAfterMessage
				&& fMessageInPosition >= fMessageIn.size())
				_Disconnect();
			return true;
		case 2:		// WAIT RESELECT
			if ((fRegs[ISTAT] & ISTAT_SIGP) != 0) {
				_SetReg32(DSP, alternate);
				return true;
			}
			// Our targets never disconnect, so nothing reselects.
			fWait = kWaitReselect;
			return false;
		case 3:		// SET
		case 4:		// CLEAR
		{
			bool set = opcode == 3;
			if ((first & (1u << 10)) != 0)
				fCarry = set;
			if ((first & (1u << 9)) != 0)
				fRegs[SCNTL0] = (fRegs[SCNTL0] & ~1) | (set ? 1 : 0);
			if ((first & (1u << 3)) != 0)
				fAttention = set;
			if ((first & (1u << 6)) != 0 && !set && fAckHeld)
				_TargetAckReleased();
			return true;
		}
	}
	_DmaInterrupt(DSTAT_IID);
	return false;
}


bool
Ncr53c810::_ReadWrite(u32 first)
{
	int opcode = (first >> 27) & 7;
	int op = (first >> 24) & 7;
	u32 reg = (first >> 16) & 0x7f;
	u8 data = (u8)(first >> 8);
	u8 operand;
	switch (opcode) {
		case 5:		// move from SFBR to register
			operand = fRegs[SFBR];
			break;
		default:	// to SFBR (6) and read-modify-write (7)
			operand = ReadRegister(reg);
			break;
	}
	u32 result;
	switch (op) {
		case 0: result = data; break;
		case 1:
			result = ((u32)operand << 1) | (fCarry ? 1 : 0);
			fCarry = (operand & 0x80) != 0;
			break;
		case 2: result = operand | data; break;
		case 3: result = operand ^ data; break;
		case 4: result = operand & data; break;
		case 5:
			result = (operand >> 1) | (fCarry ? 0x80 : 0);
			fCarry = (operand & 1) != 0;
			break;
		case 6:
			result = (u32)operand + data;
			fCarry = result > 0xff;
			break;
		default:
			result = (u32)operand + data + (fCarry ? 1 : 0);
			fCarry = result > 0xff;
			break;
	}
	if (opcode == 6)
		fRegs[SFBR] = (u8)result;
	else
		WriteRegister(reg, (u8)result);
	return true;
}


bool
Ncr53c810::_TransferControl(u32 first, u32 second)
{
	int opcode = (first >> 27) & 7;
	int phase = (first >> 24) & 7;
	bool relative = (first & (1u << 23)) != 0;
	bool carryTest = (first & (1u << 21)) != 0;
	bool interruptOnTheFly = (first & (1u << 20)) != 0;
	bool jumpIfTrue = (first & (1u << 19)) != 0;
	bool compareData = (first & (1u << 18)) != 0;
	bool comparePhase = (first & (1u << 17)) != 0;
	u8 mask = (u8)(first >> 8);
	u8 data = (u8)first;

	if (opcode > 3) {
		_DmaInterrupt(DSTAT_IID);
		return false;
	}
	bool condition = true;
	if (carryTest) {
		condition = fCarry;
	} else {
		if (comparePhase)
			condition = fTarget >= 0 && (int)fPhase == phase;
		if (compareData)
			condition = condition
				&& (fRegs[SFBR] & ~mask) == (data & ~mask);
	}
	bool taken = condition == jumpIfTrue;
	if (!carryTest && !comparePhase && !compareData)
		taken = jumpIfTrue;

	u32 dsp = _Reg32(DSP);
	u32 target = relative ? dsp + (((s32)(second << 8)) >> 8) : second;
	if (!taken)
		return true;
	switch (opcode) {
		case 0:		// JUMP
			_SetReg32(DSP, target);
			return true;
		case 1:		// CALL
			_SetReg32(TEMP, dsp);
			_SetReg32(DSP, target);
			return true;
		case 2:		// RETURN
			_SetReg32(DSP, _Reg32(TEMP));
			return true;
		default:	// INT
			if (interruptOnTheFly) {
				fRegs[ISTAT] |= ISTAT_INTF;
				_UpdateIrq();
				return true;
			}
			_DmaInterrupt(DSTAT_SIR);
			return false;
	}
}


bool
Ncr53c810::_MemoryMove(u32 first, u32 source, u32 destination)
{
	if ((first & 0x3f000000) != 0 || (source & 3) != (destination & 3)) {
		_DmaInterrupt(DSTAT_IID);
		return false;
	}
	u32 count = first & 0xffffff;
	std::vector<u8> buffer(count);
	_Read(source, buffer.data(), count);
	_Write(destination, buffer.data(), count);
	return true;
}

}	// namespace bebox
