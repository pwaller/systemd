/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#pragma once

typedef struct Tunnel Tunnel;

#include "networkd-netdev.h"

struct Tunnel {
        NetDev meta;

        int family;

        unsigned ttl;
        unsigned tos;
        union in_addr_union local;
        union in_addr_union remote;
        bool pmtudisc;
};

extern const NetDevVTable ipip_vtable;
extern const NetDevVTable sit_vtable;
extern const NetDevVTable vti_vtable;
extern const NetDevVTable gre_vtable;
