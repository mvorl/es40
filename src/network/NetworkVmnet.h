/* ES40 Emulator.
 *
 * WWW    : https://github.com/ES40-Emu/es40
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might serve
 * the general public.
 *
 */

/**
 * \file
 * vmnet network backend
 *
 * Host support:
 *  - macOS: 
 **/
#ifndef INCLUDED_NETWORK_VMNET_H_
#define INCLUDED_NETWORK_VMNET_H_

#include "NetworkBackend.h"

#if defined(HAVE_VMNET)

class CNetworkVmnetIPC;

class CNetworkVmnet: public CNetworkBackend {
public:
	CNetworkVmnet();
	virtual ~CNetworkVmnet();

	virtual bool init(const char *devid_string, CConfigurator *cfg);
	virtual int  send(const u8 *data, int len);
	virtual int  receive(const u8 **data, int *len);
	virtual void set_filter(u8 mac_list[][6], int num_macs,
	                        bool promiscuous, bool receive_all,
	                        bool pass_multicast, bool inverse);
	virtual void close();

private:
	void report_dead(const char *why);

	CNetworkVmnetIPC *ipc;
	uint64_t          max_packet_size;
	void             *rx_buf;
	bool              dead;
	const char       *devid_for_log;
};

#endif /* defined(HAVE_VMNET)  */

#endif /* !defined(INCLUDED_NETWORK_VMNET_H_)  */
