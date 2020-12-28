// Copyright (C) 2016  CBH <maodatou88@163.com>
// Licensed under the terms of the GNU GPL, version 3
// http://www.gnu.org/licenses/gpl-3.0.txt

#include "be2_val.hpp"

#include <cstdio>
#include <iostream>
#include <fstream>
#include <forward_list>
#include <cstdint>
#include <string.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <zlib.h>
#include "util.hpp"
#include "elf.h"

template <typename InputIterator>
uint32_t crc32_rpx(uint32_t crc, InputIterator first, InputIterator last);

typedef struct {
	Elf32_Ehdr ehdr;
	typedef struct {
		Elf32_Shdr hdr;
		std::vector<uint8_t> data;
		uint32_t crc32;
	} Section;
	std::vector<Section> sections;
	std::forward_list<size_t> section_file_order;
} Elf32;


void writeelf(const Elf32& elf, std::ostream& os) {
	//write elf header out
	os.write((char*)&elf.ehdr, sizeof(elf.ehdr));

	auto shdr_pad = elf.ehdr.e_shentsize - sizeof(elf.sections[0].hdr);

	os.seekp(elf.ehdr.e_shoff.value());
	for (const auto& section : elf.sections) {
		os_write_advance(section.hdr, os);

		if (shdr_pad) os.seekp(shdr_pad, std::ios_base::cur);
	}

	//these variables are a bit weird but it's optimisation I swear
	uint32_t file_offset;
	uint32_t size;
	for (auto section_index : elf.section_file_order) {
		const auto& section = elf.sections[section_index];

		file_offset = section.hdr.sh_offset.value();
		os.seekp(file_offset);
		size = section.data.size();
		os.write((const char*)section.data.data(), size);
	}
	file_offset += size;
	//if file length is not aligned to 0x40...
	if (file_offset & (0x40 - 1)) {
		//pad end of file
		os.seekp(alignup(file_offset, 0x40) - 1);
		os.put(0x00);
	}
}

std::optional<Elf32> readelf(std::istream& is) {
	Elf32 elf;
	is_read_advance(elf.ehdr, is);
	if (memcmp(elf.ehdr.e_ident, "\x7f""ELF", 4) != 0) {
		printf("e_ident bad!\n");
		return std::nullopt;
	}
	if (elf.ehdr.e_type != 0xFE01) {
		printf("e_type bad!\n");
		return std::nullopt;
	}

	//allocate space for section headers
	elf.sections.resize(elf.ehdr.e_shnum);
	std::cout << "shnum " << elf.ehdr.e_shnum << std::endl;
	//get padding (if any) for section header entries
	auto shdr_pad = elf.ehdr.e_shentsize - sizeof(elf.sections[0].hdr);
	//seek to first section header
	is.seekg(elf.ehdr.e_shoff.value());
	//go!
	for (auto& section : elf.sections) {
		//read in section header
		is_read_advance(section.hdr, is);

		//if the section headers are padded advance the stream past it
		if (shdr_pad) is.seekg(shdr_pad, std::ios_base::cur);
	}

	//sort by file offset, so we always seek forwards and maintain file order
	elf.section_file_order.resize(elf.sections.size());
	std::iota(elf.section_file_order.begin(), elf.section_file_order.end(), 0);
	elf.section_file_order.sort([=] (const auto& a, const auto& b) {
		return elf.sections[a].hdr.sh_offset < elf.sections[b].hdr.sh_offset;
	});

	//read section data
	for (auto& section_index : elf.section_file_order) {
		auto& section = elf.sections[section_index];
		auto& shdr = section.hdr;
		if (!shdr.sh_offset) continue;

		is.seekg(shdr.sh_offset.value());

		//allocate and read the uncompressed data
		section.data.resize(shdr.sh_size);
		is.read((char*)section.data.data(), section.data.size());
	}

	return elf;
}

void relink(Elf32& elf) {
	//some variables to keep track of the current file offset
	auto data_start = elf.ehdr.e_shoff + elf.ehdr.e_shnum * elf.ehdr.e_shentsize;
	auto file_offset = data_start;

	bool first_section = true;
	for (auto section_index : elf.section_file_order) {
		auto& section = elf.sections[section_index];
		auto& shdr = section.hdr;

		if (!shdr.sh_offset) continue;

		//this bit is replicating some interesting quirks of the original tool
		//I don't like it but I'm not one to argue too much
		if (!first_section) {
			file_offset = alignup(file_offset, 0x40);
		} else first_section = false;

		shdr.sh_offset = file_offset;
		file_offset += shdr.sh_size;
	}
}

void decompress(Elf32& elf) {
	//decompress sections
	for (auto& section : elf.sections) {
		auto& shdr = section.hdr;
		if (!shdr.sh_offset) continue;

		if (shdr.sh_flags & SHF_RPL_ZLIB) {
			//read in uncompressed size
			be2_val<uint32_t> uncompressed_sz;
			memcpy(&uncompressed_sz, section.data.data(), sizeof(uncompressed_sz));
			//calc compressed size
			auto compressed_sz = shdr.sh_size - sizeof(uncompressed_sz);

			std::cout << "section at " << std::hex << shdr.sh_addr << " size " << compressed_sz << " uncompresses to " << uncompressed_sz << std::endl;

			z_stream zstream = { 0 };
			inflateInit(&zstream);

			std::vector<uint8_t> uncompressed_data;
			uncompressed_data.reserve(uncompressed_sz);

			std::array<uint8_t, CHUNK> uncompressed_chunk;

			//pass to zlib
			zstream.avail_in = section.data.size() - 4;
			zstream.next_in = (Bytef*)section.data.data() + 4;

			//decompress chunk until zlib is happy
			int zret;
			do {
				//reset uncompressed chunk buffer
				zstream.avail_out = uncompressed_chunk.size();
				zstream.next_out = (Bytef*)uncompressed_chunk.data();
				//decompress!
				zret = inflate(&zstream, Z_NO_FLUSH);

				//get size of decompressed data
				auto uncompressed_chunk_sz = uncompressed_chunk.size() - zstream.avail_out;
				//copy uncomressed data into section.data
				std::copy(
					uncompressed_chunk.cbegin(),
					std::next(uncompressed_chunk.cbegin(), uncompressed_chunk_sz),
					std::back_inserter(uncompressed_data)
				);
				//update running crc
				section.crc32 = crc32_rpx(
					section.crc32,
					uncompressed_chunk.cbegin(),
					std::next(uncompressed_chunk.cbegin(), uncompressed_chunk_sz)
				);
			} while (zstream.avail_out == 0 && zret == Z_OK);

			inflateEnd(&zstream);
			section.data = std::move(uncompressed_data);

			//we decompressed this section, so clear the flag
			shdr.sh_flags &= ~SHF_RPL_ZLIB;

			//update other things as needed
			shdr.sh_size = uncompressed_sz;
		} else {
			//only compute crc
			section.crc32 = crc32_rpx(
				section.crc32,
				section.data.cbegin(),
				section.data.cend()
			);
		}
	}

	//relink elf to adjust file offsets
	relink(elf);
}

void compress(Elf32& elf) {
	for (auto& section : elf.sections) {
		auto& shdr = section.hdr;
		if (!shdr.sh_offset) continue;

		if (shdr.sh_type == SHT_RPL_FILEINFO || shdr.sh_type == SHT_RPL_CRCS ||
			shdr.sh_flags & SHF_RPL_ZLIB) continue;

		be2_val<uint32_t> uncompressed_sz = (uint32_t)section.data.size();

		z_stream zstream = { 0 };
		deflateInit(&zstream, LEVEL);

		//pass to zlib
		zstream.avail_in = section.data.size();
		zstream.next_in = (Bytef*)section.data.data();

		std::vector<uint8_t> compressed_data;
		compressed_data.resize(deflateBound(&zstream, zstream.avail_in) + sizeof(uncompressed_sz));

		zstream.avail_out = compressed_data.size() - sizeof(uncompressed_sz);
		zstream.next_out = (Bytef*)compressed_data.data() + sizeof(uncompressed_sz);

		//given deflateBound, this is guaranteed to succeed
		int zret = deflate(&zstream, Z_FINISH);

		compressed_data.resize(zstream.total_out + sizeof(uncompressed_sz));
		memcpy(compressed_data.data(), &uncompressed_sz, sizeof(uncompressed_sz));

		//not really sure how the original tool does this, but it sure does
		if (compressed_data.size() >= section.data.size()) continue;

		compressed_data.shrink_to_fit();
		section.data = std::move(compressed_data);
		shdr.sh_size = (uint32_t)section.data.size();

		shdr.sh_flags |= SHF_RPL_ZLIB;

		std::cout << "zret: " << zret << std::endl;
	}

	relink(elf);
}

int main(int argc, char** argv) {
	printf("hi!\n");
	std::ifstream infile("test.rpx", std::ios::binary);
	auto elf = *readelf(infile);

	decompress(elf);
	std::ofstream outfile("test.d.rpx", std::ios::binary);
	writeelf(elf, outfile);

	compress(elf);
	std::ofstream cotfile("test.c.rpx", std::ios::binary);
	writeelf(elf, cotfile);

	return 0;
}

template <typename InputIterator>
uint32_t crc32_rpx(uint32_t crc, InputIterator first, InputIterator last) {
	u32 crc_table[256];
	for(u32 i=0; i<256; i++)
	{
		u32 c = i;
		for(u32 j=0; j<8; j++)
		{
			if(c & 1)
				c = 0xedb88320L^(c>>1);
			else
				c = c>>1;
		}
		crc_table[i] = c;
	}
	return ~std::accumulate(first, last, ~crc, [&](uint32_t crc, uint8_t val) {
		return (crc >> 8) ^ crc_table[(crc^val) & 0xFF];
	});
}
