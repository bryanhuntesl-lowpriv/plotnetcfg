#include <net/if.h>
#include <stdio.h>
#include "handler.h"
#include "if.h"
#include "netns.h"
#include "utils.h"
#include "dot.h"

static void output_addresses(struct if_addr_entry *list)
{
	struct if_addr_entry *ptr;

	for (ptr = list; ptr; ptr = ptr->next) {
		printf("\\n%s", ptr->addr);
		if (ptr->peer)
			printf(" peer %s", ptr->peer);
	}
}

static void output_ifaces_pass1(struct if_entry *list)
{
	struct if_entry *ptr;

	for (ptr = list; ptr; ptr = ptr->next) {
		printf("%s [label=\"%s", ifdot(ptr), ptr->if_name);
		if (ptr->driver)
			printf(" (%s)", ptr->driver);
		output_addresses(ptr->addr);
		printf("\"");
		if (!(ptr->if_flags & IFF_UP))
			printf(",style=filled,fillcolor=\"grey\"");
		else if (!(ptr->if_flags & IFF_RUNNING))
			printf(",style=filled,fillcolor=\"pink\"");
		else
			printf(",style=filled,fillcolor=\"darkolivegreen1\"");
		printf("]\n");
	}
}

static void output_ifaces_pass2(struct if_entry *list)
{
	struct if_entry *ptr;

	for (ptr = list; ptr; ptr = ptr->next) {
		if (ptr->master) {
			printf("%s ->", ifdot(ptr));
			printf("%s\n", ifdot(ptr->master));
		}
		handler_print(ptr);
	}
}

void dot_output(struct netns_entry *root)
{
	struct netns_entry *ns;

	printf("digraph {\nnode [shape=box]\n");
	for (ns = root; ns; ns = ns->next) {
		if (ns->name)
			printf("subgraph cluster_%s {\nlabel=\"%s\"\n", ns->name, ns->name);
		output_ifaces_pass1(ns->ifaces);
		if (ns->name)
			printf("}");
	}
	for (ns = root; ns; ns = ns->next) {
		output_ifaces_pass2(ns->ifaces);
	}
	printf("}\n");
}
