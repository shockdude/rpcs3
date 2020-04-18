﻿#include "stdafx.h"
#include "utils.h"
#include "aes.h"
#include "sha1.h"
#include "key_vault.h"
#include "Utilities/StrFmt.h"
#include "Emu/System.h"
#include "Emu/VFS.h"
#include "unpkg.h"

LOG_CHANNEL(pkg_log, "PKG");

bool pkg_install(const std::string& path, atomic_t<double>& sync)
{
	if (!fs::is_file(path))
	{
		pkg_log.error("PKG file not found!");
		return false;
	}

	const std::size_t BUF_SIZE = 8192 * 1024; // 8 MB

	std::vector<fs::file> filelist;
	filelist.emplace_back(fs::file{path});
	u32 cur_file = 0;
	u64 cur_offset = 0;
	u64 cur_file_offset = 0;

	auto archive_seek = [&](const s64 new_offset, const fs::seek_mode damode = fs::seek_set)
	{
		if (damode == fs::seek_set)
			cur_offset = new_offset;
		else if (damode == fs::seek_cur)
			cur_offset += new_offset;

		u64 _offset = 0;
		for (u32 i = 0; i < filelist.size(); i++)
		{
			if (cur_offset < (_offset + filelist[i].size()))
			{
				cur_file = i;
				cur_file_offset = cur_offset - _offset;
				filelist[i].seek(cur_file_offset);
				break;
			}
			_offset += filelist[i].size();
		}
	};

	auto archive_read = [&](void* data_ptr, const u64 num_bytes)
	{
		const u64 num_bytes_left = filelist[cur_file].size() - cur_file_offset;

		// check if it continues in another file
		if (num_bytes > num_bytes_left)
		{
			filelist[cur_file].read(data_ptr, num_bytes_left);

			if ((cur_file + 1) < filelist.size())
			{
				++cur_file;
			}
			else
			{
				cur_offset += num_bytes_left;
				cur_file_offset = filelist[cur_file].size();
				return num_bytes_left;
			}
			const u64 num_read = filelist[cur_file].read(static_cast<u8*>(data_ptr) + num_bytes_left, num_bytes - num_bytes_left);
			cur_offset += (num_read + num_bytes_left);
			cur_file_offset = num_read;
			return (num_read + num_bytes_left);
		}

		const u64 num_read = filelist[cur_file].read(data_ptr, num_bytes);

		cur_offset += num_read;
		cur_file_offset += num_read;

		return num_read;
	};

	// Get basic PKG information
	PKGHeader header;

	if (archive_read(&header, sizeof(header)) != sizeof(header))
	{
		pkg_log.error("Reading PKG header: file is too short!");
		return false;
	}

	pkg_log.notice("Header: pkg_magic = 0x%x = \"%s\"", +header.pkg_magic, std::string(reinterpret_cast<const char*>(&header.pkg_magic), 4));
	pkg_log.notice("Header: pkg_type = 0x%x = %d", header.pkg_type, header.pkg_type);
	pkg_log.notice("Header: pkg_platform = 0x%x = %d", header.pkg_platform, header.pkg_platform);
	pkg_log.notice("Header: pkg_info_off = 0x%x = %d", header.pkg_info_off, header.pkg_info_off);
	pkg_log.notice("Header: pkg_info_num = 0x%x = %d", header.pkg_info_num, header.pkg_info_num);
	pkg_log.notice("Header: header_size = 0x%x = %d", header.header_size, header.header_size);
	pkg_log.notice("Header: file_count = 0x%x = %d", header.file_count, header.file_count);
	pkg_log.notice("Header: pkg_size = 0x%x = %d", header.pkg_size, header.pkg_size);
	pkg_log.notice("Header: data_offset = 0x%x = %d", header.data_offset, header.data_offset);
	pkg_log.notice("Header: data_size = 0x%x = %d", header.data_size, header.data_size);
	pkg_log.notice("Header: title_id = %s", header.title_id);
	pkg_log.notice("Header: qa_digest = 0x%x 0x%x", header.qa_digest[0], header.qa_digest[1]);
	//pkg_log.notice("Header: klicensee = 0x%x = %d", header.klicensee, header.klicensee);

	// Get extended PKG information for PSP or PSVita
	if (header.pkg_platform == PKG_PLATFORM_TYPE_PSP_PSVITA)
	{
		PKGExtHeader ext_header;

		archive_seek(PKG_HEADER_SIZE);

		if (archive_read(&ext_header, sizeof(ext_header)) != sizeof(ext_header))
		{
			pkg_log.error("Reading extended PKG header: file is too short!");
			return false;
		}

		pkg_log.notice("Extended header: magic = 0x%x = \"%s\"", +ext_header.magic, std::string(reinterpret_cast<const char*>(&ext_header.magic), 4));
		pkg_log.notice("Extended header: unknown_1 = 0x%x = %d", ext_header.unknown_1, ext_header.unknown_1);
		pkg_log.notice("Extended header: ext_hdr_size = 0x%x = %d", ext_header.ext_hdr_size, ext_header.ext_hdr_size);
		pkg_log.notice("Extended header: ext_data_size = 0x%x = %d", ext_header.ext_data_size, ext_header.ext_data_size);
		pkg_log.notice("Extended header: main_and_ext_headers_hmac_offset = 0x%x = %d", ext_header.main_and_ext_headers_hmac_offset, ext_header.main_and_ext_headers_hmac_offset);
		pkg_log.notice("Extended header: metadata_header_hmac_offset = 0x%x = %d", ext_header.metadata_header_hmac_offset, ext_header.metadata_header_hmac_offset);
		pkg_log.notice("Extended header: tail_offset = 0x%x = %d", ext_header.tail_offset, ext_header.tail_offset);
		//pkg_log.notice("Extended header: padding1 = 0x%x = %d", ext_header.padding1, ext_header.padding1);
		pkg_log.notice("Extended header: pkg_key_id = 0x%x = %d", ext_header.pkg_key_id, ext_header.pkg_key_id);
		pkg_log.notice("Extended header: full_header_hmac_offset = 0x%x = %d", ext_header.full_header_hmac_offset, ext_header.full_header_hmac_offset);
		//pkg_log.notice("Extended header: padding2 = 0x%x = %d", ext_header.padding2, ext_header.padding2);
	}

	if (header.pkg_magic != std::bit_cast<le_t<u32>>("\x7FPKG"_u32))
	{
		pkg_log.error("Not a PKG file!");
		return false;
	}

	switch (const u16 type = header.pkg_type)
	{
	case PKG_RELEASE_TYPE_DEBUG:   break;
	case PKG_RELEASE_TYPE_RELEASE: break;
	default:
	{
		pkg_log.error("Unknown PKG type (0x%x)", type);
		return false;
	}
	}

	switch (const u16 platform = header.pkg_platform)
	{
	case PKG_PLATFORM_TYPE_PS3: break;
	case PKG_PLATFORM_TYPE_PSP_PSVITA: break;
	default:
	{
		pkg_log.error("Unknown PKG platform (0x%x)", platform);
		return false;
	}
	}

	if (header.pkg_size > filelist[0].size())
	{
		// Check if multi-files pkg
		if (!path.ends_with("_00.pkg"))
		{
			pkg_log.error("PKG file size mismatch (pkg_size=0x%llx)", header.pkg_size);
			return false;
		}

		const std::string name_wo_number = path.substr(0, path.size() - 7);
		u64 cursize = filelist[0].size();
		while (cursize < header.pkg_size)
		{
			const std::string archive_filename = fmt::format("%s_%02d.pkg", name_wo_number, filelist.size());

			fs::file archive_file(archive_filename);
			if (!archive_file)
			{
				pkg_log.error("Missing part of the multi-files pkg: %s", archive_filename);
				return false;
			}

			cursize += archive_file.size();
			filelist.emplace_back(std::move(archive_file));
		}
	}

	if (header.data_size + header.data_offset > header.pkg_size)
	{
		pkg_log.error("PKG data size mismatch (data_size=0x%llx, data_offset=0x%llx, file_size=0x%llx)", header.data_size, header.data_offset, header.pkg_size);
		return false;
	}

	PKGMetaData metadata;
	std::string install_id;

	// Read title ID and use it as an installation directory
	install_id.resize(9);
	archive_seek(55);
	archive_read(&install_id.front(), install_id.size());

	archive_seek(header.pkg_info_off);

	for (u32 i = 0; i < header.pkg_info_num; i++)
	{
		struct packet_T
		{
			be_t<u32> id;
			be_t<u32> size;
		} packet;

		archive_read(&packet, sizeof(packet));

		// TODO
		switch (+packet.id)
		{
		case 0x1:
		{
			if (packet.size == sizeof(metadata.drm_type))
			{
				archive_read(&metadata.drm_type, sizeof(metadata.drm_type));
				pkg_log.notice("Metadata: DRM Type = 0x%x = %d", metadata.drm_type, metadata.drm_type);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: DRM Type size mismatch (0x%x)", packet.size);
			}

			break;
		}
		case 0x2:
		{
			if (packet.size == sizeof(metadata.content_type))
			{
				archive_read(&metadata.content_type, sizeof(metadata.content_type));
				pkg_log.notice("Metadata: Content Type = 0x%x = %d", metadata.content_type, metadata.content_type);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Content Type size mismatch (0x%x)", packet.size);
			}

			break;
		}
		case 0x3:
		{
			if (packet.size == sizeof(metadata.package_type))
			{
				archive_read(&metadata.package_type, sizeof(metadata.package_type));
				pkg_log.notice("Metadata: Package Type = 0x%x = %d", metadata.package_type, metadata.package_type);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Package Type size mismatch (0x%x)", packet.size);
			}
			break;
		}
		case 0x4:
		{
			if (packet.size == sizeof(metadata.package_size))
			{
				archive_read(&metadata.package_size, sizeof(metadata.package_size));
				pkg_log.notice("Metadata: Package Size = 0x%x = %d", metadata.package_size, metadata.package_size);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Package Size size mismatch (0x%x)", packet.size);
			}
			break;
		}
		case 0x5:
		{
			if (packet.size == sizeof(metadata.package_revision))
			{
				archive_read(&metadata.package_revision, sizeof(metadata.package_revision));
				pkg_log.notice("Metadata: Package Revision = 0x%x", metadata.package_revision, metadata.package_revision);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Package Revision size mismatch (0x%x)", packet.size);
			}
			break;
		}
		case 0x6:
		{
			metadata.title_id.resize(12);

			if (packet.size == metadata.title_id.size())
			{
				archive_read(&metadata.title_id, metadata.title_id.size());
				pkg_log.notice("Metadata: Title ID = %s", metadata.title_id);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Title ID size mismatch (0x%x)", packet.size);
			}
			break;
		}
		case 0x7:
		{
			// QA Digest (24 bytes)
			break;
		}
		case 0x8:
		{
			if (packet.size == sizeof(metadata.software_revision))
			{
				archive_read(&metadata.software_revision, sizeof(metadata.software_revision));
				pkg_log.notice("Metadata: Software Revision = 0x%x", metadata.software_revision, metadata.software_revision);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Software Revision size mismatch (0x%x)", packet.size);
			}
			break;
		}
		case 0x9:
		{
			// Unknown (8 bytes)
			break;
		}
		case 0xA:
		{
			if (packet.size > 8)
			{
				// Read an actual installation directory (DLC)
				install_id.resize(packet.size);
				archive_read(&install_id.front(), packet.size);
				install_id = install_id.c_str() + 8;
				metadata.install_dir = install_id;
				pkg_log.notice("Metadata: Install Dir = %s", metadata.install_dir);
				continue;
			}
			else
			{
				pkg_log.error("Metadata: Install Dir size mismatch (0x%x)", packet.size);
			}

			break;
		}
		case 0xB:
		{
			// Unknown (8 bytes)
			break;
		}
		case 0xC:
		{
			// Unknown
			break;
		}
		case 0xD:
		case 0xE:
		case 0xF:
		case 0x10:
		case 0x11:
		case 0x12:
		{
			// PSVita stuff
			break;
		}
		default:
		{
			pkg_log.error("Unknown packet id %d", packet.id);
			break;
		}
		}

		archive_seek(packet.size, fs::seek_cur);
	}

	// Get full path and create the directory
	const std::string dir = Emulator::GetHddDir() + "game/" + install_id + '/';

	// If false, an existing directory is being overwritten: cannot cancel the operation
	const bool was_null = !fs::is_dir(dir);

	if (!fs::create_path(dir))
	{
		pkg_log.error("Could not create the installation directory %s", dir);
		return false;
	}

	// Allocate buffer with BUF_SIZE size or more if required
	const std::unique_ptr<u128[]> buf(new u128[std::max<u64>(BUF_SIZE, sizeof(PKGEntry) * header.file_count) / sizeof(u128)]);

	// Define decryption subfunction (`psp` arg selects the key for specific block)
	auto decrypt = [&](u64 offset, u64 size, const uchar* key) -> u64
	{
		archive_seek(header.data_offset + offset);

		// Read the data and set available size
		const u64 read = archive_read(buf.get(), size);

		// Get block count
		const u64 blocks = (read + 15) / 16;

		if (header.pkg_type == PKG_RELEASE_TYPE_DEBUG)
		{
			// Debug key
			be_t<u64> input[8] =
			{
				header.qa_digest[0],
				header.qa_digest[0],
				header.qa_digest[1],
				header.qa_digest[1],
			};

			for (u64 i = 0; i < blocks; i++)
			{
				// Initialize stream cipher for current position
				input[7] = offset / 16 + i;

				union sha1_hash
				{
					u8 data[20];
					u128 _v128;
				} hash;

				sha1(reinterpret_cast<const u8*>(input), sizeof(input), hash.data);

				buf[i] ^= hash._v128;
			}
		}
		else if (header.pkg_type == PKG_RELEASE_TYPE_RELEASE)
		{
			aes_context ctx;

			// Set encryption key for stream cipher
			aes_setkey_enc(&ctx, key, 128);

			// Initialize stream cipher for start position
			be_t<u128> input = header.klicensee.value() + offset / 16;

			// Increment stream position for every block
			for (u64 i = 0; i < blocks; i++, input++)
			{
				u128 key;

				aes_crypt_ecb(&ctx, AES_ENCRYPT, reinterpret_cast<const u8*>(&input), reinterpret_cast<u8*>(&key));

				buf[i] ^= key;
			}
		}
		else
		{
			pkg_log.error("Unknown release type (0x%x)", header.pkg_type);
		}

		// Return the amount of data written in buf
		return read;
	};

	std::array<uchar, 16> dec_key;

	if (header.pkg_platform == PKG_PLATFORM_TYPE_PSP_PSVITA && metadata.content_type >= 0x15 && metadata.content_type <= 0x17)
	{
		// PSVita

		const uchar psp2t1[] = {0xE3, 0x1A, 0x70, 0xC9, 0xCE, 0x1D, 0xD7, 0x2B, 0xF3, 0xC0, 0x62, 0x29, 0x63, 0xF2, 0xEC, 0xCB};
		const uchar psp2t2[] = {0x42, 0x3A, 0xCA, 0x3A, 0x2B, 0xD5, 0x64, 0x9F, 0x96, 0x86, 0xAB, 0xAD, 0x6F, 0xD8, 0x80, 0x1F};
		const uchar psp2t3[] = {0xAF, 0x07, 0xFD, 0x59, 0x65, 0x25, 0x27, 0xBA, 0xF1, 0x33, 0x89, 0x66, 0x8B, 0x17, 0xD9, 0xEA};

		aes_context ctx;
		aes_setkey_enc(&ctx, metadata.content_type == 0x15u ? psp2t1 : metadata.content_type == 0x16u ? psp2t2 : psp2t3, 128);
		aes_crypt_ecb(&ctx, AES_ENCRYPT, reinterpret_cast<const uchar*>(&header.klicensee), dec_key.data());
		decrypt(0, header.file_count * sizeof(PKGEntry), dec_key.data());
	}
	else
	{
		std::memcpy(dec_key.data(), PKG_AES_KEY, dec_key.size());
		decrypt(0, header.file_count * sizeof(PKGEntry), header.pkg_platform == PKG_PLATFORM_TYPE_PSP_PSVITA ? PKG_AES_KEY2 : dec_key.data());
	}

	size_t num_failures = 0;

	std::vector<PKGEntry> entries(header.file_count);

	std::memcpy(entries.data(), buf.get(), entries.size() * sizeof(PKGEntry));

	for (const auto& entry : entries)
	{
		if (entry.name_size > 256)
		{
			num_failures++;
			pkg_log.error("PKG name size is too big (0x%x)", entry.name_size);
			continue;
		}

		const bool is_psp = (entry.type & PKG_FILE_ENTRY_PSP) != 0u;

		decrypt(entry.name_offset, entry.name_size, is_psp ? PKG_AES_KEY2 : dec_key.data());

		const std::string name{reinterpret_cast<char*>(buf.get()), entry.name_size};
		const std::string path = dir + vfs::escape(name);

		pkg_log.notice("Entry 0x%08x: %s", entry.type, name);

		switch (entry.type & 0xff)
		{
		case PKG_FILE_ENTRY_NPDRM:
		case PKG_FILE_ENTRY_NPDRMEDAT:
		case PKG_FILE_ENTRY_SDAT:
		case PKG_FILE_ENTRY_REGULAR:
		case PKG_FILE_ENTRY_UNK0:
		case PKG_FILE_ENTRY_UNK1:
		case 0xe:
		case 0x10:
		case 0x11:
		case 0x13:
		case 0x15:
		case 0x16:
		case 0x19:
		{
			const bool did_overwrite = fs::is_file(path);

			if (did_overwrite && !(entry.type & PKG_FILE_ENTRY_OVERWRITE))
			{
				pkg_log.notice("Didn't overwrite %s", path);
				break;
			}

			if (fs::file out{path, fs::rewrite})
			{
				bool extract_success = true;
				for (u64 pos = 0; pos < entry.file_size; pos += BUF_SIZE)
				{
					const u64 block_size = std::min<u64>(BUF_SIZE, entry.file_size - pos);

					if (decrypt(entry.file_offset + pos, block_size, is_psp ? PKG_AES_KEY2 : dec_key.data()) != block_size)
					{
						extract_success = false;
						pkg_log.error("Failed to extract file %s", path);
						break;
					}

					if (out.write(buf.get(), block_size) != block_size)
					{
						extract_success = false;
						pkg_log.error("Failed to write file %s", path);
						break;
					}

					if (sync.fetch_add((block_size + 0.0) / header.data_size) < 0.)
					{
						if (was_null)
						{
							pkg_log.error("Package installation cancelled: %s", dir);
							out.close();
							fs::remove_all(dir, true);
							return false;
						}

						// Cannot cancel the installation
						sync += 1.;
					}
				}

				if (extract_success)
				{
					if (did_overwrite)
					{
						pkg_log.warning("Overwritten file %s", path);
					}
					else
					{
						pkg_log.notice("Created file %s", path);
					}
				}
				else
				{
					num_failures++;
				}
			}
			else
			{
				num_failures++;
				pkg_log.error("Failed to create file %s", path);
			}

			break;
		}

		case PKG_FILE_ENTRY_FOLDER:
		case 0x12:
		{
			if (fs::create_dir(path))
			{
				pkg_log.notice("Created directory %s", path);
			}
			else if (fs::is_dir(path))
			{
				pkg_log.warning("Reused existing directory %s", path);
			}
			else
			{
				num_failures++;
				pkg_log.error("Failed to create directory %s", path);
			}

			break;
		}

		default:
		{
			num_failures++;
			pkg_log.error("Unknown PKG entry type (0x%x) %s", entry.type, name);
		}
		}
	}

	if (num_failures == 0)
	{
		pkg_log.success("Package successfully installed to %s", dir);
	}
	else
	{
		fs::remove_all(dir, true);
		pkg_log.error("Package installation failed: %s", dir);
	}
	return num_failures == 0;
}
