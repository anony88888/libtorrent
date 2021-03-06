/*

Copyright (c) 2003, Arvid Norberg
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

#ifndef TORRENT_FINGERPRINT_HPP_INCLUDED
#define TORRENT_FINGERPRINT_HPP_INCLUDED

#include <string>

#include "libtorrent/peer_id.hpp"

namespace libtorrent
{

	struct fingerprint
	{
		fingerprint(const char* id_string, int major, int minor, int revision, int tag)
			: major_version(major)
			, minor_version(minor)
			, revision_version(revision)
			, tag_version(tag)
		{
			assert(major >= 0 && major < 10);
			assert(minor >= 0 && minor < 10);
			assert(revision >= 0 && revision < 10);
			assert(tag >= 0 && tag < 10);
			assert(std::strlen(id_string) == 2);
			id[0] = id_string[0];
			id[1] = id_string[1];
		}

		std::string to_string() const
		{
			std::stringstream s;
			s << "-" << id[0] << id[1]
				<< major_version
				<< minor_version
				<< revision_version
				<< tag_version << "-";
			return s.str();
		}

		char id[2];
		char major_version;
		char minor_version;
		char revision_version;
		char tag_version;

	};

}

#endif // TORRENT_FINGERPRINT_HPP_INCLUDED
