/*

Copyright (c) 2003, Arvid Norberg, Daniel Wallin
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <ios>
#include <ctime>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <algorithm>
#include <set>

#include <boost/lexical_cast.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/ref.hpp>

#include "libtorrent/storage.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/peer_id.hpp"

#if defined(_MSC_VER)
#define for if (false) {} else for
#endif

namespace {

	struct lazy_hash
	{
		mutable libtorrent::sha1_hash digest;
		mutable libtorrent::hasher h;
		mutable const char* data;
		std::size_t size;

		lazy_hash(const char* data_, std::size_t size_)
			: data(data_)
			, size(size_)
		{}

		const libtorrent::sha1_hash& get() const
		{
			if (data)
			{
				h.update(data, size);
				digest = h.final();
				data = 0;
			}
			return digest;
		}
	};

	void print_bitmask(const std::vector<bool>& x)
	{
		for (std::size_t i = 0; i < x.size(); ++i)
		{
			std::cout << x[i];
		}
	}

} // namespace unnamed

namespace fs = boost::filesystem;

namespace {

	void print_to_log(const std::string& s)
	{
		static std::ofstream log("log.txt");
		log << s;
		log.flush();
	}

}

// TODO: implement fast resume. i.e. the possibility to
// supply additional information about which pieces are
// assigned to which slots.

namespace libtorrent {

	struct thread_safe_storage
	{
		thread_safe_storage(std::size_t n)
			: slots(n, false)
		{}

		boost::mutex mutex;
		boost::condition condition;
		std::vector<bool> slots;
	};

	struct slot_lock
	{
		slot_lock(thread_safe_storage& s, int slot_)
			: storage_(s)
			, slot(slot_)
		{
			boost::mutex::scoped_lock lock(storage_.mutex);

			while (storage_.slots[slot])
				storage_.condition.wait(lock);
			storage_.slots[slot] = true;
		}

		~slot_lock()
		{
			storage_.slots[slot] = false;
			storage_.condition.notify_all();
		}

		thread_safe_storage& storage_;
		int slot;
	};

	struct storage::impl : thread_safe_storage
	{
		impl(const torrent_info& info, const fs::path& path)
			: thread_safe_storage(info.num_pieces())
			, info(info)
			, save_path(path)
		{}

		impl(const impl& x)
			: thread_safe_storage(x.info.num_pieces())
			, info(x.info)
			, save_path(x.save_path)
		{}

		const torrent_info& info;
		const boost::filesystem::path save_path;
	};

	storage::storage(const torrent_info& info, const fs::path& path)
		: m_pimpl(new impl(info, path))
	{
		assert(info.begin_files() != info.end_files());
	}

	void storage::swap(storage& other)
	{
		m_pimpl.swap(other.m_pimpl);
	}

	storage::size_type storage::read(
		char* buf
	  , int slot
	  , size_type offset
  	  , size_type size)
	{
		assert(size > 0);

		slot_lock lock(*m_pimpl, slot);

		size_type start = slot * m_pimpl->info.piece_length() + offset;

		// find the file iterator and file offset
		size_type file_offset = start;
		std::vector<file>::const_iterator file_iter;

		for (file_iter = m_pimpl->info.begin_files();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
		}

		fs::ifstream in(
			m_pimpl->save_path / file_iter->path / file_iter->filename
			, std::ios_base::binary
		);

		assert(file_offset < file_iter->size);

		in.seekg(file_offset);

		assert(size_type(in.tellg()) == file_offset);

		size_type left_to_read = size;
		size_type slot_size = m_pimpl->info.piece_size(slot);

		if (offset + left_to_read > slot_size)
			left_to_read = slot_size - offset;

		assert(left_to_read >= 0);

		int result = left_to_read;
		int buf_pos = 0;

		while (left_to_read > 0)
		{
			int read_bytes = left_to_read;
			if (file_offset + read_bytes > file_iter->size)
				read_bytes = file_iter->size - file_offset;

			assert(read_bytes > 0);

			in.read(buf + buf_pos, read_bytes);

			int actual_read = in.gcount();
			assert(read_bytes == actual_read);

			left_to_read -= read_bytes;
			buf_pos += read_bytes;
			assert(buf_pos >= 0);
			file_offset += read_bytes;

			if (left_to_read > 0)
			{
				++file_iter;
				fs::path path = m_pimpl->save_path / file_iter->path / file_iter->filename;

				file_offset = 0;
				in.close();
				in.clear();
				in.open(path, std::ios_base::binary);
			}
		}

		return result;
	}

	void storage::write(const char* buf, int slot, size_type offset, size_type size)
	{
		assert(size > 0);

		slot_lock lock(*m_pimpl, slot);

		size_type start = slot * m_pimpl->info.piece_length() + offset;

		// find the file iterator and file offset
		size_type file_offset = start;
		std::vector<file>::const_iterator file_iter;

		for (file_iter = m_pimpl->info.begin_files();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
		}

		fs::path path(m_pimpl->save_path / file_iter->path / file_iter->filename);
		fs::ofstream out;

		if (fs::exists(path))
			out.open(path, std::ios_base::binary | std::ios_base::in);
		else
			out.open(path, std::ios_base::binary);

		assert(file_offset < file_iter->size);

		out.seekp(file_offset);

		assert(file_offset == out.tellp());

		size_type left_to_write = size;
		size_type slot_size = m_pimpl->info.piece_size(slot);

		if (offset + left_to_write > slot_size)
			left_to_write = slot_size - offset;

		assert(left_to_write >= 0);

		int buf_pos = 0;

		// TODO
		// handle case when we can't write size bytes.
		while (left_to_write > 0)
		{
			int write_bytes = left_to_write;
			if (file_offset + write_bytes > file_iter->size)
			{
				assert(file_iter->size > file_offset);
				write_bytes = file_iter->size - file_offset;
			}

			assert(buf_pos >= 0);
			assert(write_bytes > 0);
			out.write(buf + buf_pos, write_bytes);

			left_to_write -= write_bytes;
			buf_pos += write_bytes;
			assert(buf_pos >= 0);
			file_offset += write_bytes;
			assert(file_offset <= file_iter->size);

			if (left_to_write > 0)
			{
				++file_iter;

				assert(file_iter != m_pimpl->info.end_files());

				fs::path path = m_pimpl->save_path / file_iter->path / file_iter->filename;

				file_offset = 0;
				out.close();
				out.clear();

				if (fs::exists(path))
					out.open(path, std::ios_base::binary | std::ios_base::in);
				else
					out.open(path, std::ios_base::binary);
			}
		}
	}





	// -- piece_manager -----------------------------------------------------

	class piece_manager::impl
	{
	public:
		typedef entry::integer_type size_type;

		impl(
			const torrent_info& info
		  , const boost::filesystem::path& path);

		void check_pieces(
			boost::mutex& mutex
		  , detail::piece_checker_data& data
		  , std::vector<bool>& pieces);

		void allocate_slots(int num_slots);

		size_type read(char* buf, int piece_index, size_type offset, size_type size);
		void write(const char* buf, int piece_index, size_type offset, size_type size);

		const boost::filesystem::path& save_path() const
		{ return m_save_path; }

	private:
		// returns the slot currently associated with the given
		// piece or assigns the given piece_index to a free slot
		int slot_for_piece(int piece_index);

		void check_invariant() const;
		void debug_log() const;

		storage m_storage;

		// total number of bytes left to be downloaded
		size_type m_bytes_left;

		// a bitmask representing the pieces we have
		std::vector<bool> m_have_piece;

		const torrent_info& m_info;

		// maps piece index to slot index. -1 means the piece
		// doesn't exist
		std::vector<int> m_piece_to_slot;
		// slots that hasn't had any file storage allocated
		std::vector<int> m_unallocated_slots;
		// slots that has file storage, but isn't assigned to a piece
		std::vector<int> m_free_slots;

		// index here is a slot number in the file
		// -1 : the slot is unallocated
		// -2 : the slot is allocated but not assigned to a piece
		//  * : the slot is assigned to this piece
		std::vector<int> m_slot_to_piece;

		boost::filesystem::path m_save_path;

		mutable boost::recursive_mutex m_mutex;

		bool m_allocating;
		boost::mutex m_allocating_monitor;
		boost::condition m_allocating_condition;
	};

	piece_manager::impl::impl(
		const torrent_info& info
	  , const fs::path& save_path)
		: m_storage(info, save_path)
		, m_info(info)
		, m_save_path(save_path)
	{
	}

	piece_manager::piece_manager(
		const torrent_info& info
	  , const fs::path& save_path)
		: m_pimpl(new impl(info, save_path))
	{
	}

	piece_manager::size_type piece_manager::impl::read(
		char* buf
	  , int piece_index
	  , piece_manager::size_type offset
	  , piece_manager::size_type size)
	{
		assert(m_piece_to_slot[piece_index] >= 0);
		int slot = m_piece_to_slot[piece_index];
		return m_storage.read(buf, slot, offset, size);
	}

	piece_manager::size_type piece_manager::read(
		char* buf
	  , int piece_index
	  , piece_manager::size_type offset
	  , piece_manager::size_type size)
	{
		return m_pimpl->read(buf, piece_index, offset, size);
	}

	void piece_manager::impl::write(
		const char* buf
	  , int piece_index
	  , piece_manager::size_type offset
	  , piece_manager::size_type size)
	{
		int slot = slot_for_piece(piece_index);
		m_storage.write(buf, slot, offset, size);
	}

	void piece_manager::write(
		const char* buf
	  , int piece_index
	  , piece_manager::size_type offset
	  , piece_manager::size_type size)
	{
		m_pimpl->write(buf, piece_index, offset, size);
	}

	void piece_manager::impl::check_pieces(
		boost::mutex& mutex
	  , detail::piece_checker_data& data
	  , std::vector<bool>& pieces)
	{
		// synchronization ------------------------------------------------------
		boost::recursive_mutex::scoped_lock lock(m_mutex);
		// ----------------------------------------------------------------------

		m_allocating = false;
		m_piece_to_slot.resize(m_info.num_pieces(), -1);
		m_slot_to_piece.resize(m_info.num_pieces(), -1);

		m_bytes_left = m_info.total_size();

		const std::size_t piece_size = m_info.piece_length();
		const std::size_t last_piece_size = m_info.piece_size(
				m_info.num_pieces() - 1);

		bool changed_file = true;
		fs::ifstream in;

		std::vector<char> piece_data(m_info.piece_length());
		std::size_t piece_offset = 0;

		int current_slot = 0;
		std::size_t bytes_to_read = piece_size;
		std::size_t bytes_current_read = 0;
		std::size_t seek_into_next = 0;
		entry::integer_type filesize = 0;
		entry::integer_type start_of_read = 0;
		entry::integer_type start_of_file = 0;

		{
			boost::mutex::scoped_lock lock(mutex);
			data.progress = 0.f;
		}

		for (torrent_info::file_iterator file_iter = m_info.begin_files(),
			end_iter = m_info.end_files(); 
			file_iter != end_iter;)
		{
			{
				boost::mutex::scoped_lock lock(mutex);

				data.progress = (float)current_slot / m_info.num_pieces();
				if (data.abort)
					return;
			}

			assert(current_slot <= m_info.num_pieces());
			
			fs::path path(m_save_path / file_iter->path);

			// if the path doesn't exist, create the
			// entire directory tree
			if (!fs::exists(path))
				fs::create_directories(path);

			path /= file_iter->filename;

			if (changed_file)
			{
				in.close();
				in.clear();
				in.open(path, std::ios_base::binary);

				changed_file = false;

				bytes_current_read = seek_into_next;

				if (!in)
				{
					filesize = 0;
				}
				else
				{
					in.seekg(0, std::ios_base::end);
					filesize = in.tellg();
					in.seekg(seek_into_next, std::ios_base::beg);
				}
			}

			// we are at the start of a new piece
			// so we store the start of the piece
			if (bytes_to_read == m_info.piece_size(current_slot))
				start_of_read = current_slot * piece_size;

			std::size_t bytes_read = 0;

			if (filesize > 0)
			{
				in.read(&piece_data[piece_offset], bytes_to_read);
				bytes_read = in.gcount();
			}

			bytes_current_read += bytes_read;
			bytes_to_read -= bytes_read;

			assert(bytes_to_read >= 0);

			// bytes left to read, go on with next file
			if (bytes_to_read > 0)
			{
				if (bytes_current_read != file_iter->size)
				{
					entry::integer_type pos;
					entry::integer_type file_end = start_of_file + file_iter->size;

					for (pos = start_of_read; pos < file_end;
							pos += piece_size)
					{
						m_unallocated_slots.push_back(current_slot);
						++current_slot;
						assert(current_slot <= m_info.num_pieces());
					}

					seek_into_next = pos - file_end;
					bytes_to_read = m_info.piece_size(current_slot);
					piece_offset = 0;
				}
				else
				{
					seek_into_next = 0;
					piece_offset += bytes_read;
				}

				changed_file = true;
				start_of_file += file_iter->size;
				++file_iter;
				continue;
			}
		
			// we need to take special actions if this is 
			// the last piece, since that piece might actually 
			// be smaller than piece_size.

			lazy_hash large_digest(&piece_data[0], piece_size);
			lazy_hash small_digest(&piece_data[0], last_piece_size);
			
			const lazy_hash* digest[2] = {
				&large_digest, &small_digest
			};

			int found_piece = -1;

			for (int i = current_slot; i < m_info.num_pieces(); ++i)
			{
				if (pieces[i] && i != current_slot) continue;

				const sha1_hash& hash = digest[
					i == m_info.num_pieces() - 1]->get();

				if (hash == m_info.hash_for_piece(i))
					found_piece = i;
			}

			if (found_piece != -1)
			{
				if (pieces[found_piece])
				{
					assert(m_piece_to_slot[found_piece] != -1);
					m_slot_to_piece[m_piece_to_slot[found_piece]] = -2;
					m_free_slots.push_back(m_piece_to_slot[found_piece]);
				}
				else
				{
					m_bytes_left -= m_info.piece_size(found_piece);
				}

				m_piece_to_slot[found_piece] = current_slot;
				m_slot_to_piece[current_slot] = found_piece;
				pieces[found_piece] = true;
			}
			else
			{
				m_slot_to_piece[current_slot] = -2;

				entry::integer_type last_pos =
					m_info.total_size() - 
						m_info.piece_size(
							m_info.num_pieces() - 1);

				m_free_slots.push_back(current_slot);
			}

			// done with piece, move on to next
			piece_offset = 0;
			++current_slot;

			bytes_to_read = m_info.piece_size(current_slot);
		}

		std::cout << " m_free_slots: " << m_free_slots.size() << "\n";
		std::cout << " m_unallocated_slots: " << m_unallocated_slots.size() << "\n";
		std::cout << " num pieces: " << m_info.num_pieces() << "\n";

		std::cout << " have_pieces: ";
		print_bitmask(pieces);
		std::cout << "\n";
		std::cout << std::count(pieces.begin(), pieces.end(), true) << "\n";

		check_invariant();
	}

	void piece_manager::check_pieces(
		boost::mutex& mutex
	  , detail::piece_checker_data& data
	  , std::vector<bool>& pieces)
	{
		m_pimpl->check_pieces(mutex, data, pieces);
	}
	
	int piece_manager::impl::slot_for_piece(int piece_index)
	{
		// synchronization ------------------------------------------------------
		boost::recursive_mutex::scoped_lock lock(m_mutex);
		// ----------------------------------------------------------------------

		check_invariant();

		assert(piece_index >= 0 && piece_index < m_piece_to_slot.size());
		assert(m_piece_to_slot.size() == m_slot_to_piece.size());

		int slot_index = m_piece_to_slot[piece_index];

		if (slot_index != -1)
		{
			assert(slot_index >= 0);
			assert(slot_index < m_slot_to_piece.size());
			return slot_index;
		}

		if (m_free_slots.empty())
		{
			allocate_slots(5);
			assert(!m_free_slots.empty());
		}

		std::vector<int>::iterator iter(
			std::find(
				m_free_slots.begin()
				, m_free_slots.end()
				, piece_index));

		if (iter == m_free_slots.end())
		{
			assert(m_slot_to_piece[piece_index] != -2);
			iter = m_free_slots.end() - 1;

			// special case to make sure we don't use the last slot
			// when we shouldn't, since it's smaller than ordinary slots
			if (*iter == m_info.num_pieces() - 1 && piece_index != *iter)
			{
				if (m_free_slots.size() == 1)
					allocate_slots(5);
				assert(m_free_slots.size() > 1);
				// TODO: assumes that all allocated slots
				// are put at the end of the free_slots vector
				iter = m_free_slots.end() - 1;
			}
		}

		slot_index = *iter;
		m_free_slots.erase(iter);

		assert(m_slot_to_piece[slot_index] == -2);

		m_slot_to_piece[slot_index] = piece_index;
		m_piece_to_slot[piece_index] = slot_index;
	
		// there is another piece already assigned to
		// the slot we are interested in, swap positions
		if (slot_index != piece_index
			&& m_slot_to_piece[piece_index] >= 0)
		{
			std::stringstream s;

			s << "there is another piece at our slot, swapping..";

			s << "\n   piece_index: ";
			s << piece_index;
			s << "\n   slot_index: ";
			s << slot_index;
			s << "\n   piece at our slot: ";
			s << m_slot_to_piece[piece_index];
			s << "\n";

			int piece_at_our_slot = m_slot_to_piece[piece_index];

			print_to_log(s.str());

			debug_log();

			std::vector<char> buf(m_info.piece_length());
			m_storage.read(&buf[0], piece_index, 0, m_info.piece_length());
			m_storage.write(&buf[0], slot_index, 0, m_info.piece_length());

			std::swap(
				m_slot_to_piece[piece_index]
				, m_slot_to_piece[slot_index]);

			std::swap(
				m_piece_to_slot[piece_index]
				, m_piece_to_slot[piece_at_our_slot]);

			slot_index = piece_index;

			debug_log();
		}

		check_invariant();

		return slot_index;
	}

	void piece_manager::impl::allocate_slots(int num_slots)
	{
		{
			boost::mutex::scoped_lock lock(m_allocating_monitor);

			while (m_allocating)
				m_allocating_condition.wait(lock);

			m_allocating = true;
		}

		// synchronization ------------------------------------------------------
		boost::recursive_mutex::scoped_lock lock(m_mutex);
		// ----------------------------------------------------------------------

		check_invariant();

		namespace fs = boost::filesystem;
		
		std::cout << "allocating pieces...\n";

		std::vector<int>::iterator iter
			= m_unallocated_slots.begin();
		std::vector<int>::iterator end_iter 
			= m_unallocated_slots.end();

		const size_type piece_size = m_info.piece_length();

		std::vector<char> zeros(piece_size, 0);

		for (int i = 0; i < num_slots; ++i, ++iter)
		{
			if (iter == end_iter)
				break;

			int pos = *iter;
			int piece_pos = pos;

			int new_free_slot = pos;

			if (m_piece_to_slot[pos] != -1)
			{
				assert(m_piece_to_slot[pos] >= 0);
				m_storage.read(&zeros[0], m_piece_to_slot[pos], 0, m_info.piece_size(pos));
				new_free_slot = m_piece_to_slot[pos];
				m_slot_to_piece[pos] = pos;
				m_piece_to_slot[pos] = pos;
			}

			m_slot_to_piece[new_free_slot] = -2;
			m_free_slots.push_back(new_free_slot);

			m_storage.write(&zeros[0], pos, 0, m_info.piece_size(pos));
		}

		m_unallocated_slots.erase(m_unallocated_slots.begin(), iter);

		m_allocating = false;
		
		check_invariant();
	}

	void piece_manager::allocate_slots(int num_slots)
	{
		m_pimpl->allocate_slots(num_slots);
	}

	const boost::filesystem::path& piece_manager::save_path() const
	{
		return m_pimpl->save_path();
	}
	
	void piece_manager::impl::check_invariant() const
	{
		// synchronization ------------------------------------------------------
		boost::recursive_mutex::scoped_lock lock(m_mutex);
		// ----------------------------------------------------------------------

		for (int i = 0; i < m_info.num_pieces(); ++i)
		{
			if (m_piece_to_slot[i] != i && m_piece_to_slot[i] >= 0)
				assert(m_slot_to_piece[i] == -1);

			if (m_slot_to_piece[i] == -2)
			{
				assert(
					std::find(
						m_free_slots.begin()
						, m_free_slots.end()
						, i) != m_free_slots.end()
				);
			}
		}
	}

	void piece_manager::impl::debug_log() const
	{
		std::stringstream s;

		s << "index\tslot\tpiece\n";

		for (int i = 0; i < m_info.num_pieces(); ++i)
		{
			s << i << "\t" << m_slot_to_piece[i] << "\t";
			s << m_piece_to_slot[i] << "\n";
		}

		s << "---------------------------------\n";

		print_to_log(s.str());
	}

} // namespace libtorrent

