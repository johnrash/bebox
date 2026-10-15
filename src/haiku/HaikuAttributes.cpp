/*
 * BeBox emulator for Haiku - file attributes of shared folders.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HaikuAttributes.h"

#include <Node.h>
#include <fs_attr.h>


bool
HaikuAttributes::Read(const std::string& path,
	std::vector<bebox::BfsAttribute>& attributes)
{
	BNode node(path.c_str());
	if (node.InitCheck() != B_OK)
		return false;
	char name[B_ATTR_NAME_LENGTH];
	while (node.GetNextAttrName(name) == B_OK) {
		attr_info info;
		if (node.GetAttrInfo(name, &info) != B_OK || info.size > (16 << 20))
			continue;
		bebox::BfsAttribute attribute;
		attribute.name = name;
		attribute.type = info.type;
		attribute.data.resize((size_t)info.size);
		if (node.ReadAttr(name, info.type, 0, attribute.data.data(),
				attribute.data.size()) != (ssize_t)attribute.data.size())
			continue;
		attributes.push_back(attribute);
	}
	return true;
}


bool
HaikuAttributes::Write(const std::string& path,
	const std::vector<bebox::BfsAttribute>& attributes)
{
	BNode node(path.c_str());
	if (node.InitCheck() != B_OK)
		return false;
	for (const bebox::BfsAttribute& attribute : attributes) {
		// replace: a shorter value must not keep the old tail
		node.RemoveAttr(attribute.name.c_str());
		node.WriteAttr(attribute.name.c_str(), attribute.type, 0,
			attribute.data.data(), attribute.data.size());
	}
	return true;
}
