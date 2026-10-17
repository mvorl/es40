/* ES40 emulator.
 * Copyright (C) 2026 by gdwnldsKSC
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "StdAfx.h"
#include "DiskFile.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>

static const u64 QCOW2_COPIED = u64(1) << 63;
static const u64 QCOW2_COMPRESSED = u64(1) << 62;
static const u64 QCOW2_ZERO = 1;
static const u64 QCOW2_OFFSET_MASK = U64(0x00fffffffffffe00);
static const u64 QCOW2_DIRTY = 1;
static const u64 QCOW2_MAX_L1_BYTES = 32 * 1024 * 1024;
static const u64 QCOW2_MAX_REFCOUNT_TABLE_BYTES = 8 * 1024 * 1024;
static const u64 QCOW2_MAX_CHECK_BYTES = 64 * 1024 * 1024;

void CDiskFile::qcow2_open(const char* image_path, const char* expected_backing_format)
{
	qcow2 = std::make_unique<Qcow2State>();

	try
	{
		qcow2_open(handle, image_path, qcow2.get(), read_only);
		byte_size = static_cast<off_t_large>(qcow2->virtual_size);

		if (*myCfg->get_text_value("overlay_file", ""))
		{
			if (!qcow2->backing)
				FAILURE_1(Configuration, "%s: QCOW2: overlay_file must name an image with a backing file",
					devid_string);
			if (expected_backing_format &&
				strcmp(expected_backing_format, qcow2->backing_is_qcow2 ? "qcow2" : "raw"))
				FAILURE_1(Configuration, "%s: QCOW2: base format does not match the overlay's backing format",
					devid_string);
			std::string expected(myCfg->get_text_value("file", ""));
			std::unique_ptr<FILE, int (*)(FILE*)> backing(
				qcow2_open_backing("", expected), &fclose);
			if (file_identity(backing.get()) != qcow2->backing_identity)
				FAILURE_1(Configuration, "%s: QCOW2: file does not match the overlay's backing file",
					devid_string);
		}

		if (qcow2->incompatible & QCOW2_DIRTY)
		{
			// Metadata and the backing file are valid before changing the image.
			qcow2_set_dirty(false);
		}
	}
	catch (...)
	{
		qcow2.reset();
		throw;
	}
}

void CDiskFile::qcow2_open(FILE* image, const char* image_path,
	Qcow2State* info, bool readonly, std::vector<std::string> ancestors)
{
	if (ancestors.size() >= 32)
		FAILURE_1(IO, "%s: QCOW2: backing chain exceeds supported depth", devid_string);
	const std::string identity = file_identity(image);
	if (std::find(ancestors.begin(), ancestors.end(), identity) != ancestors.end())
		FAILURE_1(IO, "%s: QCOW2: backing chain contains the same image twice", devid_string);
	ancestors.push_back(identity);

	if (fseek_large(image, 0, SEEK_END) != 0)
		FAILURE_2(IO, "%s: QCOW2: cannot seek image: %s",
			devid_string, strerror(errno));
	const off_t_large end = ftell_large(image);
	if (end < 0)
		FAILURE_2(IO, "%s: QCOW2: cannot size image: %s",
			devid_string, strerror(errno));
	const u64 physical_size = static_cast<u64>(end);
	qcow2_parse_header(image, info, physical_size, readonly);
	// Validate all metadata before exposing any guest data or writes.
	qcow2_check_metadata(image, info, physical_size);
	if (readonly && (info->incompatible & QCOW2_DIRTY))
		FAILURE_1(IO, "%s: QCOW2: unclean read-only image; open it "
			"writable for a consistency check", devid_string);

	if (!info->backing_filename.empty())
	{
		info->backing.reset(qcow2_open_backing(image_path, info->backing_filename));
		info->backing_identity = file_identity(info->backing.get());
		if (info->backing_is_qcow2)
		{
			info->backing_qcow2 = std::make_unique<Qcow2State>();
			qcow2_open(info->backing.get(), info->backing_filename.c_str(),
				info->backing_qcow2.get(), true, ancestors);
			info->backing_size = info->backing_qcow2->virtual_size;
		}
		else
		{
			if (std::find(ancestors.begin(), ancestors.end(), info->backing_identity) != ancestors.end())
				FAILURE_1(IO, "%s: QCOW2: backing chain contains the same image twice", devid_string);
			info->backing_size = static_cast<u64>(ftell_large(info->backing.get()));
		}
		if (info->backing_size != info->virtual_size)
			FAILURE_1(IO, "%s: QCOW2: backing file size must match virtual disk size", devid_string);
	}
}

FILE* CDiskFile::qcow2_open_backing(const char* image_path,
	std::string& backing_filename)
{
	std::error_code error;
	const std::filesystem::path base = std::filesystem::absolute(
		std::filesystem::path(image_path).parent_path() / backing_filename, error);
	if (error)
		FAILURE_2(IO, "%s: QCOW2: cannot resolve backing file: %s",
			devid_string, error.message().c_str());
	backing_filename = base.string();
	std::unique_ptr<FILE, int (*)(FILE*)> backing(
		open_image(backing_filename.c_str(), "rb"), &fclose);
	if (!backing)
		FAILURE_2(IO, "%s: QCOW2: cannot open backing file: %s",
			devid_string, backing_filename.c_str());
	if (fseek_large(backing.get(), 0, SEEK_END) != 0)
		FAILURE_2(IO, "%s: QCOW2: cannot seek backing file: %s",
			devid_string, strerror(errno));
	const off_t_large end = ftell_large(backing.get());
	if (end < 0)
		FAILURE_2(IO, "%s: QCOW2: cannot size backing file: %s",
			devid_string, strerror(errno));
	return backing.release();
}

bool CDiskFile::qcow2_same_backing(const Qcow2State* left, const Qcow2State* right)
{
	while (left && right)
	{
		if (left->backing_identity != right->backing_identity ||
			left->backing_is_qcow2 != right->backing_is_qcow2)
			return false;
		left = left->backing_qcow2.get();
		right = right->backing_qcow2.get();
	}
	return !left && !right;
}

void CDiskFile::qcow2_parse_header(FILE* image, Qcow2State* info,
	u64 physical_size, bool readonly)
{
	u8 header[104] = {};

	read_file(image, 0, header, 72);

	if (get_be32(header) != 0x514649fb)
		FAILURE_1(IO, "%s: QCOW2: invalid magic (expected a QCOW2 image)",
			devid_string);

	info->version = get_be32(header + 4);

	if (info->version != 2 && info->version != 3)
		FAILURE_1(IO, "%s: QCOW2: unsupported format version",
			devid_string);

	if (!readonly && info->version != 3)
		FAILURE_1(IO, "%s: QCOW2: version 2 images are supported read-only; "
			"convert to compat=1.1 for writes",
			devid_string);

	const u32 cluster_bits = get_be32(header + 20);

	if (cluster_bits < 9 || cluster_bits > 21)
		FAILURE_1(IO, "%s: QCOW2: unsupported cluster size",
			devid_string);

	info->cluster_size = u64(1) << cluster_bits;

	const u64 virtual_size = get_be64(header + 24);

	if (!virtual_size || virtual_size % 512 ||
		virtual_size > u64((std::numeric_limits<off_t_large>::max)()))
		FAILURE_1(IO, "%s: QCOW2: virtual size must be a positive, "
			"sector-aligned 64-bit size",
			devid_string);

	info->virtual_size = virtual_size;

	if (get_be32(header + 32))
		FAILURE_1(IO, "%s: QCOW2: encrypted images are unsupported",
			devid_string);

	if (get_be32(header + 60))
		FAILURE_1(IO, "%s: QCOW2: internal snapshots are unsupported",
			devid_string);

	u32 header_length = 72;

	if (info->version == 3)
	{
		read_file(image, 72, header + 72, 32);
		info->incompatible = get_be64(header + 72);

		if (info->incompatible & ~QCOW2_DIRTY)
			FAILURE_1(IO, "%s: QCOW2: corrupt image or unsupported "
				"incompatible features",
				devid_string);

		if (get_be64(header + 80))
			FAILURE_1(IO, "%s: QCOW2: compatible feature flags (including lazy "
				"refcounts) are unsupported",
				devid_string);

		if (get_be64(header + 88))
			FAILURE_1(IO, "%s: QCOW2: autoclear feature flags are unsupported",
				devid_string);

		if (get_be32(header + 96) != 4)
			FAILURE_1(IO, "%s: QCOW2: only 16-bit refcounts are supported",
				devid_string);

		header_length = get_be32(header + 100);
		if (header_length < 104 || header_length > info->cluster_size || header_length % 8)
			FAILURE_1(IO, "%s: QCOW2: invalid header length",
				devid_string);
	}

	const u64 backing_offset = get_be64(header + 8);
	const u32 backing_size = get_be32(header + 16);
	if (backing_offset || backing_size)
	{
		if (!backing_offset || !backing_size || backing_size > 1023 ||
			backing_offset < header_length || backing_offset > info->cluster_size ||
			backing_size > info->cluster_size - backing_offset ||
			backing_offset > physical_size || backing_size > physical_size - backing_offset)
			FAILURE_1(IO, "%s: QCOW2: invalid backing filename bounds",
				devid_string);
	}

	// Unknown harmless extensions are preserved because header updates
	// touch only flags.
	u64 pos = header_length;
	const u64 extension_end = backing_offset ? backing_offset : info->cluster_size;
	bool found_end = false;
	bool found_backing_format = false;

	while (pos + 8 <= extension_end)
	{
		u8 ext[8];

		read_file(image, pos, ext, 8);

		const u32 kind = get_be32(ext), length = get_be32(ext + 4);

		pos += 8;

		if (!kind)
		{
			if (length)
				FAILURE_1(IO, "%s: QCOW2: invalid extension terminator",
					devid_string);
			found_end = true;
			break;
		}

		const u64 padded = (u64(length) / 8 + (length % 8 != 0)) * 8;

		if (padded > extension_end - pos)
			FAILURE_1(IO, "%s: QCOW2: header extension exceeds extension area",
				devid_string);

		if (kind == 0xe2792aca && backing_offset)
		{
			if (found_backing_format || (length != 3 && length != 5))
				FAILURE_1(IO, "%s: QCOW2: expected a single raw or qcow2 backing format extension",
					devid_string);
			u8 format[5];
			read_file(image, pos, format, length);
			if (length == 5 && !memcmp(format, "qcow2", 5))
				info->backing_is_qcow2 = true;
			else if (length != 3 || memcmp(format, "raw", 3))
				FAILURE_1(IO, "%s: QCOW2: only raw and qcow2 backing files are supported",
					devid_string);
			found_backing_format = true;
		}

		if (kind == 0x23852875 || kind == 0x0537be77 || kind == 0x44415441)
			FAILURE_1(IO, "%s: QCOW2: bitmap, encryption or external-data "
				"extension is unsupported",
				devid_string);

		pos += padded;
	}

	if (!found_end)
		FAILURE_1(IO, "%s: QCOW2: missing header extension terminator",
			devid_string);

	if (backing_offset)
	{
		if (!found_backing_format)
			FAILURE_1(IO, "%s: QCOW2: backing file must explicitly specify raw or qcow2 format",
				devid_string);
		info->backing_filename.resize(backing_size);
		read_file(image, backing_offset, &info->backing_filename[0], backing_size);
		if (info->backing_filename.find('\0') != std::string::npos)
			FAILURE_1(IO, "%s: QCOW2: backing filename contains a NUL byte",
				devid_string);
	}

	const u32 l1_entries = get_be32(header + 36);

	info->l1_offset = get_be64(header + 40);
	info->refcount_table_offset = get_be64(header + 48);
	info->refcount_table_clusters = get_be32(header + 56);

	const u64 l1_bytes = u64(l1_entries) * 8;
	const u64 l2_coverage = info->cluster_size * (info->cluster_size / 8);

	if (!l1_bytes || l1_bytes > QCOW2_MAX_L1_BYTES ||
		l1_entries < virtual_size / l2_coverage + (virtual_size % l2_coverage != 0))
		FAILURE_1(IO, "%s: QCOW2: invalid or excessive L1 table size",
			devid_string);

	const u64 refcount_bytes = info->refcount_table_clusters * info->cluster_size;

	if (!refcount_bytes || refcount_bytes > QCOW2_MAX_REFCOUNT_TABLE_BYTES)
		FAILURE_1(IO, "%s: QCOW2: invalid or excessive refcount table size",
			devid_string);

	// The table owns whole clusters, but its tail need not be
	// physically present until another allocation extends the file.
	std::vector<u8> bytes;
	auto load_table = [&](u64 offset, u64 count, std::vector<u64>& table)
		{
			if (offset < info->cluster_size || offset % info->cluster_size ||
				offset > physical_size || count > physical_size - offset)
				FAILURE_1(IO, "%s: QCOW2: metadata table outside image or "
					"misaligned",
					devid_string);

			bytes.resize(static_cast<size_t>(count));
			read_file(image, offset, bytes.data(), bytes.size());
			table.resize(bytes.size() / 8);

			for (size_t i = 0; i < table.size(); ++i)
				table[i] = get_be64(bytes.data() + i * 8);
		};

	load_table(info->l1_offset, l1_bytes, info->l1);
	load_table(info->refcount_table_offset, refcount_bytes, info->refcount_table);
}

void CDiskFile::qcow2_check_metadata(FILE* image, Qcow2State* info, u64 physical_size)
{
	const u64 clusters = physical_size / info->cluster_size + (physical_size % info->cluster_size != 0);
	const u64 bitmap_bytes = clusters / 8 + (clusters % 8 != 0);

	if (bitmap_bytes > QCOW2_MAX_CHECK_BYTES)
		FAILURE_1(IO, "%s: QCOW2: image exceeds consistency-check capacity",
			devid_string);

	std::vector<u8> reached(static_cast<size_t>(bitmap_bytes), 0);
	auto mark = [&](u64 offset, u64 count, bool data)
		
		{
			if (offset % info->cluster_size)
				FAILURE_1(IO, "%s: QCOW2: misaligned cluster reference",
					devid_string);
			
			const u64 first = offset / info->cluster_size;
			
			if (first >= clusters || count > clusters - first)
				FAILURE_1(IO, "%s: QCOW2: cluster reference outside image",
					devid_string);
			
			if (!data && (first + count) * info->cluster_size > physical_size)
				FAILURE_1(IO, "%s: QCOW2: truncated metadata cluster",
					devid_string);
			
			for (u64 i = first; i < first + count; ++i)
			{
				const u8 bit = static_cast<u8>(1u << (i % 8));
				if (reached[static_cast<size_t>(i / 8)] & bit)
					FAILURE_1(IO, "%s: QCOW2: shared clusters or overlapping "
						"metadata/data are unsupported",
						devid_string);
				reached[static_cast<size_t>(i / 8)] |= bit;
			}
		};

	mark(0, 1, false);
	
	const u64 l1_bytes = info->l1.size() * 8;
	
	mark(info->l1_offset, l1_bytes / info->cluster_size + (l1_bytes % info->cluster_size != 0), true);
	mark(info->refcount_table_offset, info->refcount_table_clusters, false);
	
	for (u64 offset : info->refcount_table)
	{
		if (!offset)
			continue;
		
		if (offset & ~QCOW2_OFFSET_MASK)
			FAILURE_1(IO, "%s: QCOW2: invalid refcount block offset",
				devid_string);
		
		mark(offset, 1, false);
	}
	
	std::vector<u8> block(static_cast<size_t>(info->cluster_size));
	const u64 guest_clusters = info->virtual_size / info->cluster_size + (info->virtual_size % info->cluster_size != 0);
	
	for (size_t i = 0; i < info->l1.size(); ++i)
	{
		const u64 entry = info->l1[i];
		
		if (entry & ~(QCOW2_OFFSET_MASK | QCOW2_COPIED))
			FAILURE_1(IO, "%s: QCOW2: reserved bits in L1 entry",
				devid_string);
		
		const u64 offset = entry & QCOW2_OFFSET_MASK;
		
		if (!offset)
		{
			if (entry)
				FAILURE_1(IO, "%s: QCOW2: invalid unallocated L1 entry",
					devid_string);
			continue;
		}
		
		mark(offset, 1, false);
		read_file(image, offset, block.data(), block.size());
		
		for (u64 j = 0; j < info->cluster_size / 8; ++j)
		{
			const u64 value = get_be64(block.data() + j * 8);
			
			if (value & QCOW2_COMPRESSED)
				FAILURE_1(IO, "%s: QCOW2: compressed clusters are unsupported",
					devid_string);
			
			if (value & ~(QCOW2_OFFSET_MASK | QCOW2_COPIED | (info->version == 3 ? QCOW2_ZERO : 0)))
				FAILURE_1(IO, "%s: QCOW2: reserved bits in L2 entry",
					devid_string);
			
			if (i * (info->cluster_size / 8) + j >= guest_clusters && value)
				FAILURE_1(IO, "%s: QCOW2: allocated cluster beyond virtual "
					"disk",
					devid_string);
			
			const u64 data = value & QCOW2_OFFSET_MASK;
			
			if (data)
				mark(data, 1, true);
			
			else if (value & QCOW2_COPIED)
				FAILURE_1(IO, "%s: QCOW2: copied flag without an allocated "
					"cluster",
					devid_string);
		}
	}
	
	// Compare every stored count, not only counts of guest-data clusters.
	const u64 refs_per_block = info->cluster_size / 2;
	
	if (clusters > info->refcount_table.size() * refs_per_block)
		FAILURE_1(IO, "%s: QCOW2: refcount table cannot cover image",
			devid_string);
	
	for (size_t i = 0; i < info->refcount_table.size(); ++i)
	{
		const u64 first = i * refs_per_block;
		
		if (!info->refcount_table[i] && first >= clusters)
			continue;
		
		if (info->refcount_table[i])
		{
			read_file(image, info->refcount_table[i], block.data(), block.size());
		}
		else
			memset(block.data(), 0, block.size());
		
		for (u64 j = 0; j < refs_per_block; ++j)
		{
			const u64 index = first + j;
			const u16 expected = index < clusters &&
				(reached[static_cast<size_t>(index / 8)] & (1u << (index % 8))) ? 1 : 0;
			
			if (get_be16(block.data() + j * 2) != expected)
				FAILURE_1(IO, "%s: QCOW2: inconsistent refcounts; "
					"inspect/repair the image offline before opening",
					devid_string);
		}
	}
}

u64 CDiskFile::qcow2_read_l2_entry(FILE* image, Qcow2State* info, u64 index, u64* table)
{
	const u64 l2_entries = info->cluster_size / 8;
	const u64 l2 = info->l1[static_cast<size_t>(index / l2_entries)] & QCOW2_OFFSET_MASK;
	
	if (table)
		*table = l2;
	
	if (!l2)
		return 0;
	
	u8 data[8];
	
	read_file(image, l2 + (index % l2_entries) * 8, data, sizeof(data));
	
	return get_be64(data);
}

void CDiskFile::qcow2_read(FILE* image, Qcow2State* info, u64 offset, void* out, size_t count)
{
	if (info->failed)
		FAILURE_1(IO, "%s: QCOW2: image is unavailable after an I/O failure; "
			"close and inspect it",
			devid_string);
	
	if (offset > info->virtual_size || u64(count) > info->virtual_size - offset)
		FAILURE_1(InvalidArgument, "%s: QCOW2: I/O outside virtual disk",
			devid_string);
	
	u8* data = static_cast<u8*>(out);
	
	while (count)
	{
		const size_t chunk = static_cast<size_t>(std::min<u64>(count, info->cluster_size - offset % info->cluster_size));
		const u64 entry = qcow2_read_l2_entry(image, info, offset / info->cluster_size);
		
		if (entry & QCOW2_ZERO)
			memset(data, 0, chunk);
		else if (entry & QCOW2_OFFSET_MASK)
		{
			const u64 host = (entry & QCOW2_OFFSET_MASK) + offset % info->cluster_size;
			// A partially stored final data cluster has an implicit zero tail.
			const size_t stored = read_file(image, host, data, chunk, false);
			if (stored < chunk)
				memset(data + stored, 0, chunk - stored);
		}
		else if (info->backing)
		{
			if (info->backing_qcow2)
				qcow2_read(info->backing.get(), info->backing_qcow2.get(), offset, data, chunk);
			else
				read_file(info->backing.get(), offset, data, chunk);
		}
		else
			memset(data, 0, chunk);
		
		offset += chunk;
		data += chunk;
		count -= chunk;
	}
}

void CDiskFile::qcow2_set_dirty(bool dirty)
{
	// Make pending metadata durable before marking the image clean.
	if (!dirty)
		sync_image();
	
	const u64 flags = dirty ? qcow2->incompatible | QCOW2_DIRTY :
		qcow2->incompatible & ~QCOW2_DIRTY;
	
	if (flags == qcow2->incompatible)
		return;
	
	u8 bytes[8];
	
	put_be64(bytes, flags);
	write_file(handle, 72, bytes, sizeof(bytes));
	sync_image();
	qcow2->incompatible = flags;
}

u64 CDiskFile::qcow2_allocate_cluster(const u8* contents)
{
	const u64 refs_per_block = qcow2->cluster_size / 2;
	
	for (;;)
	{
		if (fseek_large(handle, 0, SEEK_END) != 0)
			FAILURE_2(IO, "%s: QCOW2: cannot seek image: %s",
				devid_string, strerror(errno));
		
		const off_t_large end = ftell_large(handle);
		
		if (end < 0)
			FAILURE_2(IO, "%s: QCOW2: cannot size image: %s",
				devid_string, strerror(errno));
		
		const u64 physical_size = static_cast<u64>(end);
		const u64 cluster = physical_size / qcow2->cluster_size + (physical_size % qcow2->cluster_size != 0);
		
		if (cluster >= qcow2->refcount_table.size() * refs_per_block)
			FAILURE_1(IO, "%s: QCOW2: refcount-table allocation capacity "
				"exhausted",
				devid_string);
		
		const u64 offset = cluster * qcow2->cluster_size;
		
		if (offset > QCOW2_OFFSET_MASK)
			FAILURE_1(IO, "%s: QCOW2: refcount-table allocation capacity "
				"exhausted",
				devid_string);
		
		if (cluster >= QCOW2_MAX_CHECK_BYTES * 8)
			FAILURE_1(IO, "%s: QCOW2: image exceeds consistency-check capacity",
				devid_string);
		
		const size_t table_index = static_cast<size_t>(cluster / refs_per_block);
		
		if (!qcow2->refcount_table[table_index])
		{
			// Place the new refcount block at EOF. It accounts for itself;
			// retrying handles the case where it consumes a coverage boundary.
			std::vector<u8> refs(static_cast<size_t>(qcow2->cluster_size), 0);
			put_be16(refs.data() + (cluster % refs_per_block) * 2, 1);
			write_file(handle, offset, refs.data(), refs.size());
			sync_image();
			u8 entry[8];
			put_be64(entry, offset);
			write_file(handle, qcow2->refcount_table_offset + table_index * 8, entry, sizeof(entry));
			sync_image();
			qcow2->refcount_table[table_index] = offset;
			continue;
		}
		
		write_file(handle, offset, contents, static_cast<size_t>(qcow2->cluster_size));
		
		u8 one[2];
		
		put_be16(one, 1);
		write_file(handle, qcow2->refcount_table[table_index] + (cluster % refs_per_block) * 2, one, sizeof(one));
		sync_image();
		
		return offset;
	}
}

void CDiskFile::qcow2_write(u64 offset, const void* in, size_t count)
{
	if (qcow2->failed)
		FAILURE_1(IO, "%s: QCOW2: image is unavailable after an I/O failure; "
			"close and inspect it",
			devid_string);
	
	if (offset > u64(byte_size) || u64(count) > u64(byte_size) - offset)
		FAILURE_1(InvalidArgument, "%s: QCOW2: I/O outside virtual disk",
			devid_string);
	
	if (!count)
		return;
	
	try
	{
		const u8* data = static_cast<const u8*>(in);
		std::vector<u8> cluster;
		const u64 l2_entries = qcow2->cluster_size / 8;
		
		while (count)
		{
			const u64 index = offset / qcow2->cluster_size;
			const u64 within = offset % qcow2->cluster_size;
			const size_t chunk = static_cast<size_t>(std::min<u64>(count, qcow2->cluster_size - within));
			u64 table;
			const u64 entry = qcow2_read_l2_entry(handle, qcow2.get(), index, &table);
			
			if ((entry & QCOW2_OFFSET_MASK) && !(entry & QCOW2_ZERO))
				write_file(handle, (entry & QCOW2_OFFSET_MASK) + within, data, chunk);
			else
			{
				// Preserve inherited bytes before publishing a partial write.
				cluster.assign(static_cast<size_t>(qcow2->cluster_size), 0);
				if (qcow2->backing && chunk < qcow2->cluster_size)
				{
					const u64 start = offset - within;
					qcow2_read(handle, qcow2.get(), start, cluster.data(), static_cast<size_t>(
						std::min<u64>(qcow2->cluster_size, u64(byte_size) - start)));
				}
				memcpy(cluster.data() + within, data, chunk);
				qcow2_set_dirty(true);
				
				u8 entry_bytes[8];
				
				if (!table)
				{
					std::vector<u8> empty(static_cast<size_t>(qcow2->cluster_size), 0);
					
					table = qcow2_allocate_cluster(empty.data());
					
					const size_t l1_index = static_cast<size_t>(index / l2_entries);
					
					put_be64(entry_bytes, table | QCOW2_COPIED);
					write_file(handle, qcow2->l1_offset + l1_index * 8, entry_bytes, sizeof(entry_bytes));
					sync_image();
					qcow2->l1[l1_index] = table | QCOW2_COPIED;
				}
				
				u64 host = entry & QCOW2_OFFSET_MASK;
				
				if (host)
				{
					// Allocated-zero entries may contain stale bytes. Replace
					// the entire effective cluster before clearing their flag.
					write_file(handle, host, cluster.data(), cluster.size());
					sync_image();
				}
				else
					host = qcow2_allocate_cluster(cluster.data());
				
				put_be64(entry_bytes, host | QCOW2_COPIED);
				write_file(handle, table + (index % l2_entries) * 8, entry_bytes, sizeof(entry_bytes));
				sync_image();
			}
			
			offset += chunk;
			data += chunk;
			count -= chunk;
		}
		qcow2_set_dirty(false);
	}
	catch (...)
	{
		qcow2->failed = true;
		throw;
	}
}

bool CDiskFile::qcow2_probe(FILE* image)
{
	if (fseek_large(image, 0, SEEK_END) != 0)
		FAILURE_2(IO, "%s: QCOW2: cannot seek image: %s",
			devid_string, strerror(errno));
	
	const off_t_large end = ftell_large(image);
	
	if (end < 0)
		FAILURE_2(IO, "%s: QCOW2: cannot size image: %s",
			devid_string, strerror(errno));
	
	const u64 physical_size = static_cast<u64>(end);
	u8 header[104] = {};
	
	if (physical_size < 72)
		return false;
	
	read_file(image, 0, header, 72);
	
	const u32 version = get_be32(header + 4);
	
	if (get_be32(header) != 0x514649fb || (version != 2 && version != 3))
		return false;
	
	const u32 bits = get_be32(header + 20);
	
	if (bits < 9 || bits > 63)
		return false;
	
	const u64 cluster = u64(1) << bits;
	
	u32 length = 72;
	
	if (version == 3)
	{
		if (physical_size < sizeof(header))
			return false;
		
		read_file(image, 72, header + 72, 32);
		
		length = get_be32(header + 100);
		
		if (length < 104 || length % 8 || length > cluster || get_be32(header + 96) > 6)
			return false;
	}

	const u64 l1_offset = get_be64(header + 40), refs_offset = get_be64(header + 48);
	const u32 l1_size = get_be32(header + 36);
	
	if ((l1_size && (l1_offset < cluster || l1_offset % cluster)) ||
		refs_offset < cluster || refs_offset % cluster || get_be32(header + 56) == 0)
		return false;
	
	const u64 size = get_be64(header + 24);
	const u64 guest_clusters = size / cluster + (size % cluster != 0);
	const u64 l2_entries = cluster / 8;
	
	if (l1_size < guest_clusters / l2_entries + (guest_clusters % l2_entries != 0))
		return false;
	
	const u64 backing_offset = get_be64(header + 8);
	const u32 backing_size = get_be32(header + 16);
	
	if ((backing_offset == 0) != (backing_size == 0))
		return false;
	
	if (backing_size && (backing_size > 1023 || backing_offset < length ||
		backing_offset > cluster || backing_size > cluster - backing_offset))
		return false;
	// Incompatible flags, encryption, damaged tables, and truncated data are
	// opening errors for a recognized image, never treat it as raw.
	return true;
}

void CDiskFile::qcow2_create(u64 size, const char* backing_file, const char* backing_format)
{
	const u64 cluster_size = 65536;
	const u64 data_clusters = size / cluster_size + (size % cluster_size != 0);
	const u64 l2_entries = cluster_size / 8;
	const u64 l1_entries = data_clusters / l2_entries + (data_clusters % l2_entries != 0);
	const u64 l1_bytes = l1_entries * 8;
	
	if (l1_bytes > QCOW2_MAX_L1_BYTES)
		FAILURE_1(IO, "%s: QCOW2: virtual disk exceeds supported L1 capacity",
			devid_string);
	
	const u64 l1_clusters = l1_bytes / cluster_size + (l1_bytes % cluster_size != 0);
	const u64 refs_per_block = cluster_size / 2;
	
	u64 refcount_blocks = 1, refcount_table_clusters = 1;
	
	// Reserve enough refcounts for all guest data and L2 tables, including the
	// refcount metadata itself. The data clusters remain unallocated.
	for (unsigned iteration = 0; iteration < 32; ++iteration)
	{
		const u64 maximum = data_clusters + l1_entries + l1_clusters +
			1 + refcount_blocks + refcount_table_clusters;
		const u64 next_blocks = maximum / refs_per_block + (maximum % refs_per_block != 0);
		const u64 table_bytes = next_blocks * 8;
		const u64 next_table = table_bytes / cluster_size + (table_bytes % cluster_size != 0);
		
		if (next_blocks == refcount_blocks && next_table == refcount_table_clusters)
			break;
		refcount_blocks = next_blocks;
		refcount_table_clusters = next_table;
		
		if (iteration == 31)
			FAILURE_1(IO, "%s: QCOW2: metadata sizing did not converge",
				devid_string);
	}
	
	if (refcount_table_clusters * cluster_size > QCOW2_MAX_REFCOUNT_TABLE_BYTES)
		FAILURE_1(IO, "%s: QCOW2: virtual disk exceeds supported "
			"refcount-table capacity",
			devid_string);
	
	const u64 l1_offset = (1 + refcount_table_clusters) * cluster_size;
	const u64 refs_offset = l1_offset + l1_clusters * cluster_size;
	const u64 metadata_clusters = 1 + refcount_table_clusters + l1_clusters + refcount_blocks;
	
	if (metadata_clusters + data_clusters + l1_entries > QCOW2_MAX_CHECK_BYTES * 8)
		FAILURE_1(IO, "%s: QCOW2: virtual disk exceeds consistency-check "
			"capacity",
			devid_string);

	std::vector<u8> block(static_cast<size_t>(cluster_size), 0);
	
	put_be32(block.data(), 0x514649fb);
	put_be32(block.data() + 4, 3);
	put_be32(block.data() + 20, 16);
	put_be64(block.data() + 24, size);
	put_be32(block.data() + 36, static_cast<u32>(l1_entries));
	put_be64(block.data() + 40, l1_offset);
	put_be64(block.data() + 48, cluster_size);
	put_be32(block.data() + 56, static_cast<u32>(refcount_table_clusters));
	put_be64(block.data() + 72, QCOW2_DIRTY);
	put_be32(block.data() + 96, 4);
	put_be32(block.data() + 100, 104);
	if (*backing_file)
	{
		const u32 length = static_cast<u32>(strlen(backing_file));
		put_be64(block.data() + 8, 128);
		put_be32(block.data() + 16, length);
		put_be32(block.data() + 104, 0xe2792aca);
		const u32 format_length = static_cast<u32>(strlen(backing_format));
		put_be32(block.data() + 108, format_length);
		memcpy(block.data() + 112, backing_format, format_length);
		// The zero extension terminator occupies bytes 120 through 127.
		memcpy(block.data() + 128, backing_file, length);
	}
	write_file(handle, 0, block.data(), block.size());
	sync_image();
	
	for (u64 i = 0; i < refcount_table_clusters; ++i)
	{
		memset(block.data(), 0, block.size());
		for (u64 j = 0; j < cluster_size / 8; ++j)
		{
			const u64 index = i * (cluster_size / 8) + j;
			
			if (index < refcount_blocks)
				put_be64(block.data() + j * 8, refs_offset + index * cluster_size);
		}
		write_file(handle, (1 + i) * cluster_size, block.data(), block.size());
	}
	
	memset(block.data(), 0, block.size());
	
	for (u64 i = 0; i < l1_clusters; ++i)
	{
		write_file(handle, l1_offset + i * cluster_size, block.data(), block.size());
	}
	
	for (u64 i = 0; i < refcount_blocks; ++i)
	{
		memset(block.data(), 0, block.size());
		
		for (u64 j = 0; j < cluster_size / 2; ++j)
			if (i * (cluster_size / 2) + j < metadata_clusters)
				put_be16(block.data() + j * 2, 1);
		
		write_file(handle, refs_offset + i * cluster_size, block.data(), block.size());
	}
	
	sync_image();
	put_be64(block.data(), 0);
	write_file(handle, 72, block.data(), 8);
	sync_image();
}
