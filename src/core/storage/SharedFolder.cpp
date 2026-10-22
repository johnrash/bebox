/*
 * BeBox emulator - a host folder shared with BeOS as a SCSI disk.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "SharedFolder.h"

#include "BfsReader.h"
#include "DiskImage.h"
#include "Qcow2Image.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace bebox {

namespace {

constexpr u32
Code(const char (&c)[5])
{
	return ((u32)(u8)c[0] << 24) | ((u32)(u8)c[1] << 16)
		| ((u32)(u8)c[2] << 8) | (u32)(u8)c[3];
}


bool
HostIsLittleEndian()
{
	u16 value = 1;
	return *(u8*)&value == 1;
}


s64
ModifiedNs(const struct stat& st)
{
	return (s64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
}


u64
HashAttributes(std::vector<BfsAttribute> attributes)
{
	std::sort(attributes.begin(), attributes.end(),
		[](const BfsAttribute& a, const BfsAttribute& b) {
			return a.name < b.name;
		});
	u64 hash = 14695981039346656037ull;
	auto mix = [&hash](const void* data, size_t length) {
		const u8* bytes = (const u8*)data;
		for (size_t i = 0; i < length; i++)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
	};
	for (const BfsAttribute& attribute : attributes) {
		mix(attribute.name.data(), attribute.name.size() + 1);
		mix(&attribute.type, sizeof(attribute.type));
		u64 size = attribute.data.size();
		mix(&size, sizeof(size));
		mix(attribute.data.data(), attribute.data.size());
	}
	return hash;
}


std::string
Escape(const std::string& text)
{
	std::string result;
	for (char c : text) {
		if (c == '\\')
			result += "\\\\";
		else if (c == '\t')
			result += "\\t";
		else if (c == '\n')
			result += "\\n";
		else
			result += c;
	}
	return result;
}


std::string
Unescape(const std::string& text)
{
	std::string result;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			char c = text[++i];
			result += c == 't' ? '\t' : c == 'n' ? '\n' : c;
		} else
			result += text[i];
	}
	return result;
}


std::string
Leaf(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


// "NAME (BeBox)", numbered when that exists too.
std::string
ConflictName(const std::string& path)
{
	std::string base = path + " (BeBox)";
	struct stat st;
	if (lstat(base.c_str(), &st) != 0)
		return base;
	for (int i = 2; i < 1000; i++) {
		std::string name = path + " (BeBox " + std::to_string(i) + ")";
		if (lstat(name.c_str(), &st) != 0)
			return name;
	}
	return base;
}

}	// namespace


SharedFolder::SharedFolder(const std::string& folder, const std::string& image,
	HostAttributes* attributes)
	:
	fFolder(folder),
	fImage(image),
	fAttributes(attributes)
{
	while (fFolder.size() > 1 && fFolder.back() == '/')
		fFolder.pop_back();
}


void
SharedFolder::SwapAttribute(u32 type, std::vector<u8>& data)
{
	size_t element = 0;
	size_t limit = data.size();
	switch (type) {
		case Code("SHRT"):
		case Code("USHT"):
			element = 2;
			break;
		case Code("LONG"):
		case Code("ULNG"):
		case Code("FLOT"):
		case Code("RECT"):
		case Code("BPNT"):
		case Code("APPF"):
			element = 4;
			break;
		case Code("LLNG"):
		case Code("ULLG"):
		case Code("DBLE"):
		case Code("OFFT"):
			element = 8;
			break;
		case Code("TIME"):
			element = data.size() == 8 ? 8 : 4;
			break;
		case Code("APPV"):
			// version_info: five uint32, then two strings
			element = 4;
			limit = std::min<size_t>(limit, 20);
			break;
		default:
			return;
	}
	for (size_t i = 0; i + element <= limit; i += element)
		std::reverse(data.begin() + i, data.begin() + i + element);
}


std::vector<BfsAttribute>
SharedFolder::_HostAttributes(const std::string& path)
{
	std::vector<BfsAttribute> attributes;
	if (fAttributes == nullptr || !fAttributes->Read(path, attributes))
		return std::vector<BfsAttribute>();
	if (HostIsLittleEndian()) {
		for (BfsAttribute& attribute : attributes)
			SwapAttribute(attribute.type, attribute.data);
	}
	return attributes;
}


void
SharedFolder::_WriteHostAttributes(const std::string& path,
	std::vector<BfsAttribute> attributes)
{
	if (fAttributes == nullptr || attributes.empty())
		return;
	if (HostIsLittleEndian()) {
		for (BfsAttribute& attribute : attributes)
			SwapAttribute(attribute.type, attribute.data);
	}
	fAttributes->Write(path, attributes);
}


// #pragma mark - building the volume


bool
SharedFolder::_ScanFolder(const std::string& relative, BfsWriter::Node& node,
	u64& bytes, std::string& error)
{
	std::string path = fFolder + relative;
	DIR* dir = opendir(path.c_str());
	if (dir == nullptr) {
		error = "cannot read the folder " + path;
		return false;
	}
	std::vector<std::string> names;
	while (struct dirent* entry = readdir(dir)) {
		std::string name = entry->d_name;
		if (name != "." && name != ".." && name.size() <= 255)
			names.push_back(name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());

	for (const std::string& name : names) {
		std::string childRelative = relative + "/" + name;
		std::string full = fFolder + childRelative;
		struct stat st;
		if (lstat(full.c_str(), &st) != 0)
			continue;
		BfsWriter::Node child;
		child.name = name;
		child.permissions = st.st_mode & 07777;
		child.modified = st.st_mtim.tv_sec;
		child.created = st.st_mtim.tv_sec;
		Record record;
		record.hostModified = ModifiedNs(st);
		if (S_ISDIR(st.st_mode)) {
			child.directory = true;
			record.directory = true;
			child.attributes = _HostAttributes(full);
			if (!_ScanFolder(childRelative, child, bytes, error))
				return false;
		} else if (S_ISREG(st.st_mode)) {
			child.size = (u64)st.st_size;
			record.hostSize = child.size;
			child.attributes = _HostAttributes(full);
			child.read = [full](u64 offset, void* buffer, size_t length) {
				int fd = open(full.c_str(), O_RDONLY);
				if (fd < 0)
					return false;
				size_t done = 0;
				bool ok = true;
				while (done < length) {
					ssize_t got = pread(fd, (u8*)buffer + done, length - done,
						(off_t)(offset + done));
					if (got < 0 && errno == EINTR)
						continue;
					if (got < 0)
						ok = false;
					if (got <= 0)
						break;
					done += (size_t)got;
				}
				close(fd);
				// a file that shrank meanwhile reads as zeros (an error
				// fails the volume: "cannot read NAME")
				memset((u8*)buffer + done, 0, length - done);
				return ok;
			};
		} else
			continue;		// symbolic links and special files stay out
		bytes += (child.size + 2 * BfsWriter::kBlockSize) & ~(u64)2047;
		for (const BfsAttribute& attribute : child.attributes)
			bytes += attribute.data.size() + BfsWriter::kBlockSize;
		fRecords.push_back(std::make_pair(childRelative, record));
		node.children.push_back(std::move(child));
	}
	return true;
}


// The snapshot entries: what the volume holds as written.
void
SharedFolder::_Record(const std::string& relative,
	const BfsWriter::Node& node)
{
	std::unordered_map<std::string, size_t> index;
	for (size_t i = 0; i < fRecords.size(); i++)
		index[fRecords[i].first] = i;
	std::function<void(const std::string&, const BfsWriter::Node&)> record
		= [&](const std::string& parent, const BfsWriter::Node& directory) {
		for (const BfsWriter::Node& child : directory.children) {
			std::string childRelative = parent + "/" + child.name;
			auto found = index.find(childRelative);
			if (found != index.end()) {
				Record& entry = fRecords[found->second].second;
				entry.bfsModified = child.storedModified;
				entry.bfsSize = child.size;
				entry.attributesHash = HashAttributes(child.attributes);
			}
			if (child.directory)
				record(childRelative, child);
		}
	};
	record(relative, node);
}


bool
SharedFolder::_Build(std::string& error)
{
	struct stat st;
	if (stat(fFolder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
		error = "the shared folder " + fFolder + " does not exist";
		return false;
	}
	fRecords.clear();
	BfsWriter::Node root;
	root.directory = true;
	root.name = Leaf(fFolder);
	root.permissions = st.st_mode & 07777;
	root.modified = root.created = st.st_mtim.tv_sec;
	root.attributes = _HostAttributes(fFolder);
	u64 bytes = 0;
	if (!_ScanFolder("", root, bytes, error))
		return false;

	// room to grow: twice the content and 512 MB, at least 1 GB
	u64 size = std::max<u64>(1ull << 30, bytes * 2 + (512ull << 20));
	size = (size + (1 << 20) - 1) & ~(u64)((1 << 20) - 1);
	unlink(fImage.c_str());
	if (!Qcow2Image::Create(fImage, size, error))
		return false;
	std::unique_ptr<DiskImage> image = DiskImage::Open(fImage, false, error);
	if (!image)
		return false;
	std::string name = root.name.substr(0, 31);
	if (!BfsWriter::Write([&image](u64 offset, const void* buffer,
				size_t length) {
				return image->Write(offset, buffer, length);
			}, size, name, root, error)) {
		image.reset();
		unlink(fImage.c_str());
		return false;
	}
	image->Flush();
	_Record("", root);
	Record rootRecord;
	rootRecord.directory = true;
	rootRecord.attributesHash = HashAttributes(root.attributes);
	fRecords.push_back(std::make_pair(std::string(), rootRecord));
	if (!_SaveSnapshot()) {
		error = "cannot write " + fImage + ".snapshot";
		return false;
	}
	return true;
}


// Written next to the old one, then renamed: a snapshot is whole or the
// old one.
bool
SharedFolder::_SaveSnapshot()
{
	std::string path = fImage + ".snapshot";
	std::string temporary = path + ".new";
	FILE* file = fopen(temporary.c_str(), "w");
	if (file == nullptr)
		return false;
	fprintf(file, "BeBox shared folder 1\t%s\n", Escape(fFolder).c_str());
	for (const auto& item : fRecords) {
		const Record& record = item.second;
		fprintf(file, "%c\t%s\t%llu\t%lld\t%lld\t%llu\t%llu\n",
			record.directory ? 'D' : 'F', Escape(item.first).c_str(),
			(unsigned long long)record.hostSize,
			(long long)record.hostModified, (long long)record.bfsModified,
			(unsigned long long)record.bfsSize,
			(unsigned long long)record.attributesHash);
	}
	bool ok = !ferror(file);
	ok = fclose(file) == 0 && ok;
	if (!ok || rename(temporary.c_str(), path.c_str()) != 0) {
		unlink(temporary.c_str());
		return false;
	}
	return true;
}


bool
SharedFolder::_LoadSnapshot()
{
	fRecords.clear();
	std::string path = fImage + ".snapshot";
	FILE* file = fopen(path.c_str(), "r");
	if (file == nullptr)
		return false;
	std::string text;
	char buffer[65536];
	size_t got;
	while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, got);
	fclose(file);
	size_t position = text.find('\n');
	if (text.compare(0, 22, "BeBox shared folder 1\t") != 0
		|| position == std::string::npos)
		return false;
	// the folder the volume was made from (the setting may have changed)
	fSnapshotFolder = Unescape(text.substr(22, position - 22));
	position++;
	while (position < text.size()) {
		size_t end = text.find('\n', position);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(position, end - position);
		position = end + 1;
		std::vector<std::string> fields;
		size_t start = 0;
		for (;;) {
			size_t tab = line.find('\t', start);
			fields.push_back(line.substr(start, tab == std::string::npos
				? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}
		if (fields.size() != 7)
			continue;
		Record record;
		record.directory = fields[0] == "D";
		record.hostSize = strtoull(fields[2].c_str(), nullptr, 10);
		record.hostModified = strtoll(fields[3].c_str(), nullptr, 10);
		record.bfsModified = strtoll(fields[4].c_str(), nullptr, 10);
		record.bfsSize = strtoull(fields[5].c_str(), nullptr, 10);
		record.attributesHash = strtoull(fields[6].c_str(), nullptr, 10);
		fRecords.push_back(std::make_pair(Unescape(fields[1]), record));
	}
	return true;
}


bool
SharedFolder::Prepare(std::string& report, std::string& error)
{
	report.clear();
	struct stat st;
	bool leftOver = stat((fImage + ".snapshot").c_str(), &st) == 0
		&& stat(fImage.c_str(), &st) == 0;
	if (leftOver) {
		std::string syncReport;
		std::string syncError;
		if (!Sync(syncReport, syncError)) {
			// not cleanly unmounted: BeOS gets the volume back to repair
			if (_LoadSnapshot()) {
				report = syncError;
				return true;
			}
		} else
			report = syncReport;
	}
	return _Build(error);
}


// #pragma mark - syncing back


bool
SharedFolder::Sync(std::string& report, std::string& error)
{
	report.clear();
	if (!_LoadSnapshot()) {
		error = "no snapshot of the shared folder " + fFolder;
		return false;
	}
	std::unique_ptr<DiskImage> image = DiskImage::Open(fImage, true, error);
	if (!image)
		return false;
	std::string currentFolder = fFolder;
	fFolder = fSnapshotFolder;
	bool result = _Sync(*image, report, error);
	fFolder = currentFolder;
	image.reset();
	if (result) {
		unlink((fImage + ".snapshot").c_str());
		unlink(fImage.c_str());
	}
	return result;
}


bool
SharedFolder::_Sync(DiskImage& volume, std::string& report,
	std::string& error)
{
	DiskImage* image = &volume;
	BfsReader reader([&image](u64 offset, void* buffer, size_t length) {
		return image->Read(offset, buffer, length);
	});
	std::vector<u64> volumes = reader.FindVolumes(std::vector<u64>());
	if (volumes.empty() || !reader.Mount(volumes[0])) {
		error = "the shared volume of " + fFolder + " is damaged";
		return false;
	}
	if (!reader.IsClean()) {
		error = "BeOS did not shut down cleanly: the changes to the shared "
			"folder " + fFolder + " come back at the next power on, after "
			"BeOS has repaired the volume.";
		return false;
	}

	std::map<std::string, Record> records(fRecords.begin(), fRecords.end());
	std::set<std::string> seen;
	int copied = 0;
	int deleted = 0;
	std::vector<std::string> conflicts;
	// what could not be read from the volume or written to the folder
	std::vector<std::string> failures;
	auto failed = [&failures](const std::string& what, int code) {
		failures.push_back(code != 0 ? what + ": " + strerror(code) : what);
	};
	std::vector<u8> buffer(1 << 20);

	std::function<void(u64, const std::string&)> walk
		= [&](u64 directory, const std::string& relative) {
		BfsReader::RawEntries entries;
		if (!reader.ListBlock(directory, entries)) {
			failed("cannot read the directory " + (relative.empty() ? "/"
				: relative) + " on the volume", 0);
			return;
		}
		for (const auto& item : entries) {
			std::string childRelative = relative + "/" + item.first;
			std::string host = fFolder + childRelative;
			BfsReader::Stat stat;
			std::vector<BfsAttribute> attributes;
			if (!reader.StatInode(item.second, stat)) {
				failed("cannot read " + childRelative + " on the volume", 0);
				continue;
			}
			reader.ReadAttributes(item.second, attributes);
			u64 hash = HashAttributes(attributes);
			seen.insert(childRelative);
			auto record = records.find(childRelative);
			struct stat hostStat;
			bool hostExists = lstat(host.c_str(), &hostStat) == 0;
			if (stat.directory) {
				if (!hostExists) {
					if (mkdir(host.c_str(), (stat.mode & 0777) | 0700) != 0) {
						failed(host, errno);
						continue;
					}
					_WriteHostAttributes(host, attributes);
				} else if (record == records.end()
					|| record->second.attributesHash != hash)
					_WriteHostAttributes(host, attributes);
				Record& done = records[childRelative];
				done.directory = true;
				done.attributesHash = hash;
				walk(item.second, childRelative);
				// Tracker gives every volume a trash: leave it out while it
				// is empty
				if (!hostExists && (childRelative == "/home"
						|| childRelative == "/home/Desktop"
						|| childRelative == "/home/Desktop/Trash"))
					rmdir(host.c_str());
				continue;
			}
			bool changed = record == records.end()
				|| record->second.directory
				|| record->second.bfsModified != stat.modified
				|| record->second.bfsSize != stat.size;
			if (!changed) {
				if (record->second.attributesHash != hash) {
					_WriteHostAttributes(host, attributes);
					record->second.attributesHash = hash;
				}
				continue;
			}
			std::string target = host;
			if (hostExists && (record == records.end()
					|| (u64)hostStat.st_size != record->second.hostSize
					|| ModifiedNs(hostStat) != record->second.hostModified)) {
				// changed on both sides: the host keeps its version
				target = ConflictName(host);
			}
			std::string temporary = target + ".bebox-sync";
			int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
				(stat.mode & 0777) | 0600);
			if (fd < 0) {
				failed(target, errno);
				continue;
			}
			bool readOk = true;
			int writeError = 0;
			for (u64 offset = 0; offset < stat.size; ) {
				size_t length = (size_t)std::min<u64>(buffer.size(),
					stat.size - offset);
				if (!reader.ReadAt(item.second, offset, buffer.data(),
						length)) {
					readOk = false;
					break;
				}
				ssize_t written = write(fd, buffer.data(), length);
				if (written != (ssize_t)length) {
					writeError = written < 0 ? errno : ENOSPC;
					break;
				}
				offset += length;
			}
			if (close(fd) != 0 && writeError == 0)
				writeError = errno;
			if (readOk && writeError == 0
				&& rename(temporary.c_str(), target.c_str()) != 0)
				writeError = errno;
			if (!readOk || writeError != 0) {
				unlink(temporary.c_str());
				if (!readOk)
					failed("cannot read " + childRelative + " on the volume", 0);
				else
					failed(target, writeError);
				continue;
			}
			struct timeval times[2];
			times[0].tv_sec = times[1].tv_sec
				= (time_t)BfsWriter::FromBfsTime(stat.modified);
			times[0].tv_usec = times[1].tv_usec = 0;
			utimes(target.c_str(), times);
			_WriteHostAttributes(target, attributes);
			copied++;
			if (target != host)
				conflicts.push_back(Leaf(target));
			// as synced (a conflict leaves the host's file as it was)
			Record& done = records[childRelative];
			if (target == host && lstat(host.c_str(), &hostStat) == 0) {
				done.hostSize = (u64)hostStat.st_size;
				done.hostModified = ModifiedNs(hostStat);
			}
			done.directory = false;
			done.bfsModified = stat.modified;
			done.bfsSize = stat.size;
			done.attributesHash = hash;
		}
	};
	walk(reader.RootBlock(), "");

	// the folder's own attributes (Tracker's window state, for one)
	std::vector<BfsAttribute> rootAttributes;
	reader.ReadAttributes(reader.RootBlock(), rootAttributes);
	auto rootRecord = records.find("");
	if (rootRecord != records.end()
		&& rootRecord->second.attributesHash != HashAttributes(rootAttributes))
		_WriteHostAttributes(fFolder, rootAttributes);

	if (!failures.empty()) {
		// No deletions: what was not seen may still be on the volume, and
		// a file BeOS moved may not be at its new place yet. The volume
		// stays (BeOS gets it back at the next power on); the snapshot
		// takes what went to the folder, so that the next sync does not
		// take it for a conflict.
		fRecords.assign(records.begin(), records.end());
		_SaveSnapshot();
		error = "Not all of BeOS's changes to the shared folder " + fFolder
			+ " could be made (" + failures[0];
		if (failures.size() > 1)
			error += ", and " + std::to_string(failures.size() - 1)
				+ " more";
		error += "). The volume is kept; the sync is tried again at the "
			"next power on.";
		return false;
	}

	// deleted in BeOS: files unchanged on the host since power on, then
	// emptied directories (deepest first)
	std::vector<std::string> gone;
	for (const auto& item : records) {
		if (!item.first.empty() && seen.count(item.first) == 0)
			gone.push_back(item.first);
	}
	std::sort(gone.begin(), gone.end(), [](const std::string& a,
			const std::string& b) {
		return a.size() > b.size();
	});
	for (const std::string& relative : gone) {
		const Record& record = records[relative];
		std::string host = fFolder + relative;
		struct stat hostStat;
		if (lstat(host.c_str(), &hostStat) != 0)
			continue;
		if (record.directory) {
			if (S_ISDIR(hostStat.st_mode) && rmdir(host.c_str()) == 0)
				deleted++;
		} else if ((u64)hostStat.st_size == record.hostSize
			&& ModifiedNs(hostStat) == record.hostModified
			&& unlink(host.c_str()) == 0)
			deleted++;
	}

	if (copied > 0 || deleted > 0 || !conflicts.empty()) {
		report = "Shared folder \"" + Leaf(fFolder) + "\": "
			+ std::to_string(copied) + " file(s) from BeOS, "
			+ std::to_string(deleted) + " removed";
		if (!conflicts.empty()) {
			report += "; changed on both sides, BeOS's version kept as \""
				+ conflicts[0] + "\"";
			if (conflicts.size() > 1)
				report += " and " + std::to_string(conflicts.size() - 1)
					+ " more";
		}
		report += ".";
	}
	return true;
}

}	// namespace bebox
