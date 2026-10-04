/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * http_reachable - see http_reachable.h for why this exists.
 */

#include "http_reachable.h"

#include <wx/uri.h>

/*
 * The crash this avoids needs both the curl backend and the GTK event loop,
 * which is wxGTK and nothing else: Windows uses WinHTTP and macOS uses
 * NSURLSession, and neither goes near the code at fault. Testing for the
 * toolkit rather than for Linux is what makes that exact - a GTK build on any
 * other Unix has the same problem, and a Mac told to use the curl backend by
 * hand would too.
 *
 * Everywhere else this compiles to false and the caller is unchanged.
 */
#ifdef __WXGTK__

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

/* Long enough to reach a host that is merely slow, short enough not to be felt
   before a download the user has just asked for. A host that takes longer than
   this is not refusing the connection, which is the only thing being looked
   for, so giving up here reports nothing and costs nothing. */
const int CONNECT_TIMEOUT_MS = 2000;

/**
 * Connect to one resolved address, and say whether it was refused.
 *
 * @param refused Set true only when the connection was actively refused
 * @return        true if the attempt reached a conclusion
 */
bool TryOne(const struct addrinfo *ai, bool &refused)
{
	const int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
	    ai->ai_protocol);

	if (fd < 0) {
		return false;
	}

	int err = 0;

	if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
		/* Refused hosts do not do this. */
		close(fd);
		return true;
	}

	if (errno != EINPROGRESS) {
		err = errno;
	} else {
		struct pollfd pfd;

		pfd.fd = fd;
		pfd.events = POLLOUT;
		pfd.revents = 0;

		const int ready = poll(&pfd, 1, CONNECT_TIMEOUT_MS);

		if (ready <= 0) {
			/* Timed out or interrupted: not a refusal, and not
			   something to hold the user up over. */
			close(fd);
			return false;
		}

		socklen_t len = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
			close(fd);
			return false;
		}
	}

	close(fd);

	if (err == ECONNREFUSED) {
		refused = true;
	}
	return true;
}

} /* namespace */

bool HttpConnectionRefused(const wxString &url)
{
	const wxURI uri(url);

	if (!uri.HasServer()) {
		return false;
	}

	const wxString scheme = uri.GetScheme().Lower();
	wxString port = uri.GetPort();

	if (port.empty()) {
		/* wxURI leaves the port empty when the URL relies on the
		   scheme's default, and getaddrinfo needs it spelled out. */
		if (scheme == "https") {
			port = "443";
		} else if (scheme == "http") {
			port = "80";
		} else {
			return false;	/* not ours to guess at */
		}
	}

	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *list = nullptr;

	if (getaddrinfo(uri.GetServer().utf8_str().data(),
	        port.utf8_str().data(), &hints, &list) != 0 || list == nullptr) {
		/* A name that does not resolve is a failure wxWebRequest
		   survives and explains, so let it. */
		return false;
	}

	bool refused = false;

	for (const struct addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
		bool this_refused = false;

		if (!TryOne(ai, this_refused)) {
			continue;
		}
		if (!this_refused) {
			/* One address answering is enough: the request will
			   have somewhere to go. */
			freeaddrinfo(list);
			return false;
		}
		refused = true;
	}

	freeaddrinfo(list);

	/* Only when every address that reached a conclusion refused. A host
	   with both an A and a AAAA record, one of which is listening, is
	   reachable. */
	return refused;
}

#else

bool HttpConnectionRefused(const wxString &url)
{
	(void) url;
	return false;
}

#endif /* __WXGTK__ */
