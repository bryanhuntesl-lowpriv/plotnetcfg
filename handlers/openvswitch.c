#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/un.h>
#include <unistd.h>
#include "../handler.h"
#include "../if.h"
#include "../netns.h"
#include "../utils.h"
#include "../parson/parson.h"
#include "openvswitch.h"

static char db[] = "/var/run/openvswitch/db.sock";

struct ovs_if {
	struct ovs_if *next;
	struct if_entry *link;
	unsigned int if_index;
	char *name;
	char *type;
	/* for vxlan: */
	char *local_ip;
	char *remote_ip;
};

struct ovs_bridge {
	struct ovs_bridge *next;
	char *name;
	struct ovs_if *ifaces;
	struct ovs_if *system;
};

static struct ovs_bridge *br_list;

static int is_set(JSON_Array *j)
{
	return (!strcmp(json_array_get_string(j, 0), "set"));
}

static int is_map(JSON_Array *j)
{
	return (!strcmp(json_array_get_string(j, 0), "map"));
}

static int is_uuid(JSON_Array *j)
{
	return (!strcmp(json_array_get_string(j, 0), "uuid"));
}

static int is_empty(JSON_Value *j)
{
	return json_type(j) == JSONArray && is_set(json_array(j));
}

static struct ovs_if *parse_iface(JSON_Object *jresult, JSON_Array *uuid)
{
	struct ovs_if *iface;
	JSON_Object *jif;
	JSON_Value *jval;
	JSON_Array *jarr;
	unsigned int i;

	if (!is_uuid(uuid))
		return NULL;
	jif = json_object_get_object(jresult, "Interface");
	jif = json_object_get_object(jif, json_array_get_string(uuid, 1));
	jif = json_object_get_object(jif, "new");

	iface = calloc(sizeof(*iface), 1);
	if (!iface)
		return NULL;
	jval = json_object_get_value(jif, "ifindex");
	if (!is_empty(jval))
		iface->if_index = json_number(jval);
	iface->name = strdup(json_object_get_string(jif, "name"));
	iface->type = strdup(json_object_get_string(jif, "type"));
	jarr = json_object_get_array(jif, "options");
	if (!strcmp(iface->type, "vxlan") && is_map(jarr)) {
		jarr = json_array_get_array(jarr, 1);
		for (i = 0; i < json_array_get_count(jarr); i++) {
			JSON_Array *jkv;
			const char *key, *val;

			jkv = json_array_get_array(jarr, i);
			key = json_array_get_string(jkv, 0);
			val = json_array_get_string(jkv, 1);
			if (!strcmp(key, "local_ip"))
				iface->local_ip = strdup(val);
			else if (!strcmp(key, "remote_ip"))
				iface->remote_ip = strdup(val);
		}
	}
	return iface;
}

static struct ovs_if *parse_port(JSON_Object *jresult, JSON_Array *uuid,
				 struct ovs_bridge *br)
{
	struct ovs_if *list = NULL, *ptr, *iface;
	JSON_Object *jport;
	JSON_Array *jarr;
	unsigned int i;

	if (!is_uuid(uuid))
		return NULL;
	jport = json_object_get_object(jresult, "Port");
	jport = json_object_get_object(jport, json_array_get_string(uuid, 1));
	jport = json_object_get_object(jport, "new");

	jarr = json_object_get_array(jport, "interfaces");
	if (is_set(jarr)) {
		jarr = json_array_get_array(jarr, 1);
		for (i = 0; i < json_array_get_count(jarr); i++) {
			iface = parse_iface(jresult, json_array_get_array(jarr, i));
			if (!iface)
				return NULL;
			if (!list)
				list = iface;
			else
				ptr->next = iface;
			ptr = iface;
		}
	} else
		list = parse_iface(jresult, jarr);

	if (!strcmp(json_object_get_string(jport, "name"), br->name))
		br->system = list;

	return list;
}

static struct ovs_bridge *parse_bridge(JSON_Object *jresult, JSON_Array *uuid)
{
	struct ovs_bridge *br;
	struct ovs_if *ptr, *iface;
	JSON_Object *jbridge;
	JSON_Array *jarr;
	unsigned int i;

	if (!is_uuid(uuid))
		return NULL;
	jbridge = json_object_get_object(jresult, "Bridge");
	jbridge = json_object_get_object(jbridge, json_array_get_string(uuid, 1));
	jbridge = json_object_get_object(jbridge, "new");

	br = calloc(sizeof(*br), 1);
	if (!br)
		return NULL;
	br->name = strdup(json_object_get_string(jbridge, "name"));
	jarr = json_object_get_array(jbridge, "ports");
	if (is_set(jarr)) {
		jarr = json_array_get_array(jarr, 1);
		for (i = 0; i < json_array_get_count(jarr); i++) {
			iface = parse_port(jresult, json_array_get_array(jarr, i), br);
			if (!iface)
				return NULL;
			if (!br->ifaces)
				br->ifaces = iface;
			else
				ptr->next = iface;
			ptr = iface;
			while (ptr->next)
				ptr = ptr->next;
		}
	} else
		br->ifaces = parse_port(jresult, jarr, br);
	if (!br->ifaces)
		return NULL;
	return br;
}

static struct ovs_bridge *parse(char *answer)
{
	struct ovs_bridge *list = NULL, *ptr, *br;
	JSON_Value *jroot;
	JSON_Object *jresult, *jovs;
	JSON_Array *jarr;
	unsigned int i;

	jroot = json_parse_string(answer);
	if (!jroot)
		return NULL;
	jresult = json_object_get_object(json_object(jroot), "result");
	if (!jresult)
		return NULL;
	/* TODO: add the rest of error handling */
	jovs = json_object_get_object(jresult, "Open_vSwitch");
	if (json_object_get_count(jovs) != 1)
		return NULL;
	jovs = json_object_get_object(jovs, json_object_get_name(jovs, 0));
	jovs = json_object_get_object(jovs, "new");

	jarr = json_object_get_array(jovs, "bridges");
	if (is_set(jarr)) {
		jarr = json_array_get_array(jarr, 1);
		for (i = 0; i < json_array_get_count(jarr); i++) {
			br = parse_bridge(jresult, json_array_get_array(jarr, i));
			if (!br)
				return NULL;
			if (!list)
				list = br;
			else
				ptr->next = br;
			ptr = br;
		}
	} else
		list = parse_bridge(jresult, jarr);
	return list;
}


static void add_table(JSON_Object *parmobj, char *table, ...)
{
	va_list ap;
	JSON_Value *new;
	JSON_Object *tableobj;
	JSON_Array *cols;
	char *s;

	va_start(ap, table);
	new = json_value_init_object();
	tableobj = json_object(new);
	json_object_set(parmobj, table, new);
	new = json_value_init_array();
	cols = json_array(new);
	json_object_set(tableobj, "columns", new);
	while ((s = va_arg(ap, char *)))
		json_array_append(cols, json_value_init_string(s));
}

static char *construct_query(void)
{
	JSON_Value *root, *new;
	JSON_Object *ro, *po;
	JSON_Array *params;
	char *res;

	root = json_value_init_object();
	ro = json_object(root);
	json_object_set(ro, "method", json_value_init_string("monitor"));
	json_object_set(ro, "id", json_value_init_number(0));

	new = json_value_init_array();
	params = json_array(new);
	json_object_set(ro, "params", new);
	json_array_append(params, json_value_init_string("Open_vSwitch"));
	json_array_append(params, json_value_init_null());
	new = json_value_init_object();
	po = json_object(new);
	json_array_append(params, new);
	add_table(po, "Open_vSwitch", "bridges", "ovs_version", NULL);
	add_table(po, "Bridge", "name", "ports");
	add_table(po, "Port", "interfaces", "name", "tag", "trunks", NULL);
	add_table(po, "Interface", "name", "ifindex", "type", "options", "admin_state", "link_state", NULL);

	res = json_serialize(root);
	json_value_free(root);
	return res;
}

static int connect_ovs(void)
{
	int fd;
	struct sockaddr_un sun;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return fd;
	sun.sun_family = AF_UNIX;
	strncpy(sun.sun_path, db, UNIX_PATH_MAX);
	sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
	connect(fd, (struct sockaddr *)&sun, sizeof(sun.sun_family) + strlen(sun.sun_path) + 1);
	return fd;
}

#define CHUNK	65536
static char *read_all(int fd)
{
	char *buf, *newbuf;
	size_t len = 0;
	ssize_t res;

	buf = malloc(CHUNK);
	if (!buf)
		return NULL;
	while (1) {
		res = read(fd, buf + len, CHUNK);
		if (res < 0) {
			free(buf);
			return NULL;
		}
		len += res;
		if (res < CHUNK) {
			buf[len] = '\0';
			return buf;
		}
		newbuf = realloc(buf, len + CHUNK);
		if (!newbuf) {
			free(buf);
			return NULL;
		}
		buf = newbuf;
	}
}

static int link_iface_search(struct if_entry *entry, void *arg)
{
	struct ovs_if *iface = arg;
	int weight;

	if (iface->if_index != entry->if_index)
		return 0;
	if (entry->master && strcmp(entry->master->if_name, "ovs-system"))
		return 0;
	weight = 1;
	if (iface->link) {
		if (iface->link->ns == entry->ns)
			weight += 2;
	} else {
		if (!entry->ns->name)
			weight += 2;
	}
	if (!strcmp(iface->name, entry->if_name))
		weight++;
	return weight;
}

static int link_iface(struct ovs_if *iface, struct netns_entry *root)
{
	int err;

	if (!iface->if_index || iface->link)
		return 0;
	err = find_interface(&iface->link, root, NULL, link_iface_search, iface);
	if (err > 0)
		return err;
	if (err < 0) {
		fprintf(stderr, "ERROR: cannot map openvswitch interface %s reliably.\n",
			iface->name);
		return EEXIST;
	}
	if (!iface->link) {
		fprintf(stderr, "ERROR: cannot map openvswitch interface %s.\n",
			iface->name);
		return ENOENT;
	}
	return 0;
}

static int link_ifaces(struct netns_entry *root)
{
	struct ovs_bridge *br;
	struct ovs_if *iface;
	int err;

	for (br = br_list; br; br = br->next) {
		if (!br->system) {
			fprintf(stderr, "ERROR: cannot find main interface for openvswitch bridge %s.\n",
				br->name);
			return ENOENT;
		}
		if ((err = link_iface(br->system, root)))
			return err;
		for (iface = br->ifaces; iface; iface = iface->next) {
			if (iface == br->system)
				continue;
			if ((err = link_iface(iface, root)))
				return err;
			if (iface->link) {
				/* reconnect to the ovs master */
				iface->link->master = br->system->link;
			}
		}
	}
	return 0;
}

static int ovs_global_post(struct netns_entry *root)
{
	char *str;
	int fd, len;
	int err;

	str = construct_query();
	len = strlen(str);
	fd = connect_ovs();
	if (fd < 0)
		return 0;
	if (write(fd, str, len) < len) {
		close(fd);
		return 0;
	}
	json_free_serialization_string(str);
	str = read_all(fd);
	br_list = parse(str);
	free(str);
	close(fd);
	if (!br_list)
		return 0;
	if ((err = link_ifaces(root)))
		return err;
	return 0;
}

static void ovs_global_print(_unused struct netns_entry *root)
{
	struct ovs_bridge *br;
	struct ovs_if *iface;
	char *system;

	for (br = br_list; br; br = br->next) {
		system = ifstr(br->system->link);
		for (iface = br->ifaces; iface; iface = iface->next) {
			if (iface->link)
				continue;
			printf("\"//ovs/%s/%s\" [label=\"%s",
			       br->name, iface->name, iface->name);
			if (iface->type && *iface->type)
				printf("\ntype: %s", iface->type);
			if (iface->local_ip)
				printf("\nfrom %s", iface->local_ip);
			if (iface->remote_ip)
				printf("\nto %s", iface->remote_ip);
			printf("\",style=dotted]\n");
			printf("\"//ovs/%s/%s\" -> \"%s\"\n",
			       br->name, iface->name, system);
		}
	}
}

static void destruct_if(struct ovs_if *iface)
{
	free(iface->name);
	free(iface->type);
	free(iface->local_ip);
	free(iface->remote_ip);
}

static void destruct_bridge(struct ovs_bridge *br)
{
	free(br->name);
	list_free(br->ifaces, (destruct_f)destruct_if);
}

static void ovs_global_cleanup(_unused struct netns_entry *root)
{
	list_free(br_list, (destruct_f)destruct_bridge);
}

static struct global_handler gh_ovs = {
	.post = ovs_global_post,
	.print = ovs_global_print,
	.cleanup = ovs_global_cleanup,
};

void handler_ovs_register(void)
{
	global_handler_register(&gh_ovs);
}