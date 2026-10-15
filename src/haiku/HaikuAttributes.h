/*
 * BeBox emulator for Haiku - file attributes of shared folders.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Gives the shared folder code (src/core/storage/SharedFolder) access to
 * Haiku's file attributes, so that file types, icons and Tracker's state
 * travel between Haiku and BeOS.
 */
#ifndef BEBOX_HAIKU_ATTRIBUTES_H
#define BEBOX_HAIKU_ATTRIBUTES_H

#include "core/storage/SharedFolder.h"

class HaikuAttributes : public bebox::HostAttributes {
public:
	virtual	bool				Read(const std::string& path,
									std::vector<bebox::BfsAttribute>&
										attributes);
	virtual	bool				Write(const std::string& path,
									const std::vector<bebox::BfsAttribute>&
										attributes);
};

#endif	// BEBOX_HAIKU_ATTRIBUTES_H
