#ifdef WANT_SYSTEMD
/*
 * systemd.h
 *
 * Copyright 2012 Werner Fink, 2012 SuSE Linux Products GmbH Nuernberg, Germany
 *
 * This source is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <dbus/dbus.h>
extern int systemd_get_tree(DBusConnection *bus);
extern DBusConnection * systemd_open_conn(void);
extern void systemd_close_conn(DBusConnection *bus);
extern void systemd_free(void);

/*
 * Linked lists of services, targets and their allies.
 */

typedef struct sdserv __align sdserv_t;
typedef struct {
    list_t	a_list;
    ushort	 flags;
    sdserv_t	 *serv;
} __align ally_t;

struct sdserv {
    list_t	s_list;
    list_t	a_list;
    char	 *unit;
    char	 *name;
};

extern list_t sdservs;

#define SDREL_REQUIRES		(1<<0)	    /* Requires/RequiresOverridable	*/
#define SDREL_REQUISITE		(1<<1)	    /* Requisite/RequisiteOverridable	*/
#define SDREL_WANTS		(1<<2)	    /* Wants		*/
#define SDREL_CONFLICTS		(1<<3)	    /* Conflicts	*/
#define	SDREL_BEFORE		(1<<4)	    /* Before		*/
#define SDREL_AFTER		(1<<5)	    /* After		*/

#endif /* WANT_SYSTEMD */

