/*
 * This file is part of bgpstream
 *
 * CAIDA, UC San Diego
 * bgpstream-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __BGPVIEW_IO_H
#define __BGPVIEW_IO_H

#include "bgpview.h"

/** Send the given view to the given socket
 *
 * @param dest          socket to send the prefix to
 * @param view          pointer to the view to send
 * @param cb            callback function to use to filter peers (may be NULL)
 * @return 0 if the view was sent successfully, -1 otherwise
 */
int bgpview_send(void *dest, bgpview_t *view,
                         bgpview_filter_peer_cb_t *cb);

/** Receive a view from the given socket
 *
 * @param src           socket to receive on
 * @param view          pointer to the clear/new view to receive into
 * @return pointer to the view instance received, NULL if an error occurred.
 */
int bgpview_recv(void *src, bgpview_t *view);

#endif /* __BGPVIEW_IO_H */