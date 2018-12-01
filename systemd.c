#ifdef WANT_SYSTEMD
/*
 * systemd.c	    Use dbus API of systemd to import dependencies from.
 *
 * Copyright 2012 Werner Fink, 2012 SUSE LINUX Products GmbH, Germany.
 *
 * This program is free software; you can redistribute it and/or modify
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

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "listing.h"
#include "systemd.h"

/*
 * Linked list of services, targets and their allies.
 */
list_t sdservs = { &sdservs, &sdservs }, *sdservs_start = &sdservs;

static sdserv_t * addsysd(const char *const name) attribute((malloc,nonnull(1)));
static sdserv_t * addsysd(const char *const name)
{
    sdserv_t *restrict this;
    const char *dot;
    boolean target;
    list_t *ptr;
    size_t len;

    list_for_each_prev(ptr, sdservs_start) {
	this = list_entry(ptr, sdserv_t, s_list);
	if (!strcmp(this->unit, name))
	    goto out;
    }

    target = false;
    len = strsize(name);
    if ((dot = strrchr(name, '.'))) {
	dot++;
	if (*dot == 't') {
	    target = true;
	    len++;
	}
    }
    if (posix_memalign((void*)&this, sizeof(void*), alignof(sdserv_t)+len) != 0)
	error ("addsysd() can not alloc memory for %s: %m\n", name);
    memset(this, 0, alignof(sdserv_t)+len);
    insert(&this->s_list, sdservs_start->prev);
    this->name = ((char*)this)+alignof(sdserv_t);
    if (target) {
	*this->name = '$';
	strcpy(this->name+1, name);
	this->unit = this->name+1;
    } else {
	strcpy(this->name, name);
	this->unit = this->name;
    }
    initial(&this->a_list);
out:
    return this;
}

static void addally(sdserv_t *restrict serv, sdserv_t *restrict ally, const ushort flags)
{
    ally_t *restrict this;
    list_t *ptr, *allies;

    allies = &serv->a_list;
    list_for_each_prev(ptr, allies) {
	this = list_entry(ptr, ally_t, a_list);
	if (this->serv == ally) {
	    this->flags |= flags;
	    if (this->flags & SDREL_WANTS) {
		if (this->flags & (SDREL_REQUIRES|SDREL_REQUISITE))
		    warn("bogus systemd dependcy: %s %s\n", serv->unit, this->serv->unit);
	    }
	    return;
	}
    }
    if (posix_memalign((void*)&this, sizeof(void*), alignof(ally_t)) != 0)
	error ("addally() can not alloc memory for %s: %m\n", ally->unit);
    memset(this, 0, alignof(ally_t));
    insert(&this->a_list, allies->prev);
    this->serv = ally;
    this->flags = flags;
}

static void systemd_strip_dot(void)
{
    list_t *ptr;

    list_for_each_prev(ptr, &sdservs) {
	sdserv_t *sdserv = list_entry(ptr, sdserv_t, s_list);
	char *dot = strrchr(sdserv->unit, '.');
	const char *tag;
	if (dot == (char*)0 || *dot == '\0')
	    continue;
	tag = dot+1;
	if (strcmp(tag, "target") == 0 || strcmp(tag, "service") == 0)
	    *dot = '\0';
	else
	    *dot = '_';
    }
}

static int iter_get_and_next(DBusMessageIter *iter, int type, void *data)
{
    if (dbus_message_iter_get_arg_type(iter) != type)
	goto err;
    dbus_message_iter_get_basic(iter, data);
    if (!dbus_message_iter_next(iter))
	goto err;
    return 1;
err:
    return 0;
}

typedef struct {
    const char *const tag;
    ushort flag;
} relation_t;

static int handle_one_property(sdserv_t *one, const char *prop, DBusMessageIter *iter)
{
    static relation_t relations[] = {
	{ "Requires",			SDREL_REQUIRES	},
	{ "RequiresOverridable",	SDREL_REQUIRES	},
	{ "Requisite",			SDREL_REQUISITE	},
	{ "RequisiteOverridable",	SDREL_REQUISITE	},
	{ "Wants",			SDREL_WANTS	},
	{ "Conflicts",			SDREL_CONFLICTS	},
	{ "Before",			SDREL_BEFORE	},
	{ "After",			SDREL_AFTER	},
	{ (const char*)0,		0		}
    };
    const relation_t *relation = (relation_t*)0;
    int i;

    for (i = 0; relations[i].tag; i++) {
	if (strcmp(relations[i].tag, prop) == 0) {
	    relation = &relations[i];
	    break;
	}
    }

    if (!relation)
	return 1;

    switch (dbus_message_iter_get_arg_type(iter)) {
    case DBUS_TYPE_ARRAY:
	if (dbus_message_iter_get_element_type(iter) == DBUS_TYPE_STRING) {
	    DBusMessageIter isub;

	    dbus_message_iter_recurse(iter, &isub);

	    do {
		const char *ally;
		const char *dot;
		int type;

		type = dbus_message_iter_get_arg_type(&isub);
		if (type == DBUS_TYPE_INVALID)
		    break;
		if (type != DBUS_TYPE_STRING)
		    goto failed;
		dbus_message_iter_get_basic(&isub, &ally);

		// Here we go to parse the dependcies
		// targets are somehow like `$'
		// services are somehow scripts
		// Wants -> Optional
		// Requires/Requisite -> Requires
		// After -> ??

		if ((dot = strrchr(ally, '.'))) {
		    dot++;
		    if (strcmp(dot, "target") == 0 || strcmp(dot, "service") == 0) {
			sdserv_t * serv;
#if 0
			printf("\tdependcy (%s)\t: \"%s\" -> \"%s\"\n", relation->tag, one->unit, ally);
#endif
			serv = addsysd(ally);
			addally(one, serv, relation->flag);
		    }
		}

		dbus_message_iter_next(&isub);

	    } while (TRUE);
	}
    }
    return 1;
failed:
    warn ("failed to parse reply from systemd\n");
    return 0;
}

static int handle_one_id(DBusConnection *bus, sdserv_t * serv, const char *path)
{
    DBusError error;
    const char *device = "org.freedesktop.systemd1.Unit";
    DBusMessage *reply, *send;
    DBusMessageIter iter, isub;
    int ret = 0;

    dbus_error_init(&error);
    send = dbus_message_new_method_call("org.freedesktop.systemd1",
					 path,
					"org.freedesktop.DBus.Properties",
					"GetAll");
    if (!send)
	goto err;
    dbus_message_set_auto_start(send, TRUE);
    if (!dbus_message_set_destination(send, "org.freedesktop.systemd1"))
	goto err;

    dbus_message_iter_init_append(send, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &device);

    reply = dbus_connection_send_with_reply_and_block(bus, send, -1, &error);
    dbus_message_unref(send);

    if (dbus_error_is_set(&error)) {
	warn ("can not connect system dbus: %s\n", error.message);
	dbus_error_free(&error);
	goto err;
    }
    if (!reply)
	goto err;
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
	goto unref;

    if (!dbus_message_iter_init(reply, &iter) ||
	dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
	dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_DICT_ENTRY)
	goto failed;
    dbus_message_iter_recurse(&iter, &isub);

    do {
	DBusMessageIter iisub, iiisub;
	const char *prop;
	int type;

	type = dbus_message_iter_get_arg_type(&isub);
	if (type == DBUS_TYPE_INVALID)
	    break;
	if (type != DBUS_TYPE_DICT_ENTRY)
	    goto failed;

	dbus_message_iter_recurse(&isub, &iisub);
	if (!iter_get_and_next(&iisub, DBUS_TYPE_STRING, &prop))
	    goto failed;
	if (dbus_message_iter_get_arg_type(&iisub) != DBUS_TYPE_VARIANT)
	    goto failed;

	dbus_message_iter_recurse(&iisub, &iiisub);

	handle_one_property(serv, prop, &iiisub);

	dbus_message_iter_next(&isub);

    } while (TRUE);

    ret = 1;
    goto unref;
failed:
    warn ("failed to parse reply from systemd\n");
unref:
    dbus_message_unref(reply);
err:
    return ret;
}

int systemd_get_tree(DBusConnection *bus)
{
    DBusError error;
    DBusMessage *reply, *send;
    DBusMessageIter iter, isub;
    int ret = 0;

    dbus_error_init(&error);
    send = dbus_message_new_method_call("org.freedesktop.systemd1",
					"/org/freedesktop/systemd1",
					"org.freedesktop.systemd1.Manager",
					"ListUnits");
    if (!send)
	goto err;
    dbus_message_set_auto_start(send, TRUE);
    if (!dbus_message_set_destination(send, "org.freedesktop.systemd1"))
	goto err;

    reply = dbus_connection_send_with_reply_and_block(bus, send, -1, &error);
    dbus_message_unref(send);

    if (dbus_error_is_set(&error)) {
	warn ("can not connect system dbus: %s\n", error.message);
	dbus_error_free(&error);
	goto err;
    }
    if (!reply)
	goto err;
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
	goto unref;

    if (!dbus_message_iter_init(reply, &iter) ||
	dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
	dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRUCT)
	goto failed;
    dbus_message_iter_recurse(&iter, &isub);

    do {
	const char *id, *description, *load_state, *active_state,
		   *sub_state, *following, *unit_path;
	DBusMessageIter iisub;
	const char *dot;
	int type;

	type = dbus_message_iter_get_arg_type(&isub);
	if (type == DBUS_TYPE_INVALID)
	    break;
	if (type != DBUS_TYPE_STRUCT)
	    goto failed;
	dbus_message_iter_recurse(&isub, &iisub);

	if (!iter_get_and_next(&iisub, DBUS_TYPE_STRING, &id) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_STRING, &description) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_STRING, &load_state) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_STRING, &active_state) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_STRING, &sub_state) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_STRING, &following) ||
	    !iter_get_and_next(&iisub, DBUS_TYPE_OBJECT_PATH, &unit_path))
	    goto failed;

#if 0
uint job_id;
const char *sjob_type;
const char *j_path;

	iter_get_and_next(&iisub, DBUS_TYPE_UINT32, &job_id);
	iter_get_and_next(&iisub, DBUS_TYPE_STRING, &sjob_type);
	iter_get_and_next(&iisub, DBUS_TYPE_OBJECT_PATH, &j_path);

printf("id=%s ls=%s as=%s subs=%s fl=%s %s ID=%u jt=%s jp=%s\n",
	id, load_state, active_state, sub_state, following, unit_path,
	job_id, sjob_type, j_path);
#endif
	if ((dot = strrchr(id, '.'))) {
	    dot++;
	    if (strcmp(dot, "target") == 0 || strcmp(dot, "service") == 0) {
		sdserv_t * serv;
		serv = addsysd(id);
		handle_one_id(bus, serv, unit_path);
	    }
	}

	dbus_message_iter_next(&isub);

    } while (TRUE);

    systemd_strip_dot();

    ret = 1;
    goto unref;
failed:
    warn ("failed to parse reply from systemd\n");
unref:
    dbus_message_unref(reply);
err:
    return ret;
}

DBusConnection * systemd_open_conn(void)
{
    DBusConnection *bus;
    DBusError error;
    struct ucred cred;
    socklen_t len;
    int oerrno;
    int DBfd;

    dbus_error_init(&error);
    if (geteuid() == 0)	    /* for root use unix socket for speed */
	bus = dbus_connection_open_private("unix:path=/run/systemd/private", &error);
    else
	bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);

    if (!bus) {
	if (errno != ENOENT)
	    warn ("can not connect systemd: %s\n", error.message);
	goto err;
    }
    dbus_connection_set_exit_on_disconnect(bus, FALSE);

    if (!dbus_connection_get_unix_fd(bus, &DBfd))
	goto perm;
    len = sizeof(cred);
    if (getsockopt(DBfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) 
	goto perm;
    oerrno = errno;
    errno = E2BIG;
    if (len != sizeof(cred))
	goto perm;
    errno = EPERM;
    if (cred.uid != 0 && cred.uid != geteuid())
	goto perm;
    errno = oerrno;
    return bus;
perm:
    warn ("can not connect to systemd: %m\n");
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
err:
    if (dbus_error_is_set(&error))
	dbus_error_free(&error);
    return (DBusConnection*)0;
}

void systemd_close_conn(DBusConnection *bus)
{
    if (bus) {
	dbus_connection_flush(bus);
	dbus_connection_close(bus);
	dbus_connection_unref(bus);
    }
}

void systemd_free(void)
{
    list_t *ptr, *safe;
    list_for_each_safe(ptr, safe, sdservs_start) {
	sdserv_t *serv = list_entry(ptr, sdserv_t, s_list);
	list_t *aptr, *asafe;
	list_for_each_safe(aptr, asafe, &serv->a_list) {
	    ally_t *ally = list_entry(aptr, ally_t, a_list);
	    delete(aptr);
	    free(ally);
	}
	delete(ptr);
	free(serv);
    }
}
#endif /* WANT_SYSTEMD */
