/*
 * BeBox emulator - an emulated machine, as the front ends see it.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Machine.h"

#include "BeBoxMachine.h"
#include "../hobbit/HobbitMachine.h"

namespace bebox {

std::unique_ptr<Machine>
Machine::Create(const MachineConfig& config)
{
	if (config.model == MachineConfig::kHobbit)
		return std::unique_ptr<Machine>(new HobbitMachine(config));
	return std::unique_ptr<Machine>(new BeBoxMachine(config));
}

}	// namespace bebox
