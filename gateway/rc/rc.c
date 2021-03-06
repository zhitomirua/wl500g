/*
 * Copyright 2004, ASUSTek Inc.
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND ASUS GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 * $Id$
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <net/if_arp.h>
#include <dirent.h>

#include <epivers.h>
#include <netconf.h>
#include <nvparse.h>
#include <bcmdevs.h>
#include <wlutils.h>
#include <bcmparams.h>
#include "mtd.h"
#include "rc.h"

static void restore_defaults(void);
static void rc_signal(int sig);

extern struct nvram_tuple router_defaults[];
extern int noconsole;

//static int restore_defaults_g = 0;
int router_model = MDL_UNKNOWN;


static int
build_ifnames(char *type, char *names, int *size)
{
	char name[32], *next;
	int len = 0;
	int s;

	/* open a raw scoket for ioctl */
	if ((s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0)
       		return -1;

	/*
	 * go thru all device names (wl<N> il<N> et<N> vlan<N>) and interfaces to 
	 * build an interface name list in which each i/f name coresponds to a device
	 * name in device name list. Interface/device name matching rule is device
	 * type dependant:
	 *
	 *	wl:	by unit # provided by the driver, for example, if eth1 is wireless
	 *		i/f and its unit # is 0, then it will be in the i/f name list if
	 *		wl0 is in the device name list.
	 *	il/et:	by mac address, for example, if et0's mac address is identical to
	 *		that of eth2's, then eth2 will be in the i/f name list if et0 is 
	 *		in the device name list.
	 *	vlan:	by name, for example, vlan0 will be in the i/f name list if vlan0
	 *		is in the device name list.
	 */
	foreach (name, type, next) {
		struct ifreq ifr;
		int i, unit;
		char var[32], *mac, ea[ETHER_ADDR_LEN];
		
		/* vlan: add it to interface name list */
		if (!strncmp(name, "vlan", 4)) {
			/* append interface name to list */
			len += snprintf(&names[len], *size - len, "%s ", name);
			continue;
		}

		/* others: proceed only when rules are met */
		for (i = 1; i <= DEV_NUMIFS; i ++) {
			/* ignore i/f that is not ethernet */
			ifr.ifr_ifindex = i;
			if (ioctl(s, SIOCGIFNAME, &ifr))
				continue;
			if (ioctl(s, SIOCGIFHWADDR, &ifr))
				continue;
			if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
				continue;
			if (!strncmp(ifr.ifr_name, "vlan", 4))
				continue;
			/* wl: use unit # to identify wl */
			if (!strncmp(name, "wl", 2)) {
				if (wl_probe(ifr.ifr_name) ||
				    wl_ioctl(ifr.ifr_name, WLC_GET_INSTANCE, &unit, sizeof(unit)) ||
				    unit != atoi(&name[2]))
					continue;
			}
			/* et/il: use mac addr to identify et/il */
			else if (!strncmp(name, "et", 2) || !strncmp(name, "il", 2)) {
				snprintf(var, sizeof(var), "%smacaddr", name);
				if (!(mac = nvram_get(var)) || !ether_atoe(mac, ea) ||
				    memcmp(ea, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN))
					continue;

				// add by Chen-I to filter out wl interface here
				if (!wl_probe(ifr.ifr_name))
					continue;

			}
			/* mac address: compare value */
			else if (ether_atoe(name, ea) && !memcmp(ea, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN))
				;
			/* others: ignore */
			else
				continue;

			/* append interface name to list */
			len += snprintf(&names[len], *size - len, "%s ", ifr.ifr_name);
		}
	}
	
	close(s);

	*size = len;
	return 0;
}	

static void
stb_set(void)
{
	int stbport = nvram_get_int("wan_stb_x");

	if (stbport < 0 || stbport > 5)
		return;
	if (stbport == 0 && !nvram_match("vlan_set_x", "1"))
		return;

	switch (router_model)
	{
		case MDL_RTN16:
		case MDL_WNR3500L:
			{
			/* Set LAN ports */
			char *vlan1ports[] = {
					"1 2 3 4 8*",	// Defaults
					"1 2 3 8*",	// LAN 2 + LAN 3 + LAN 4
					"1 2 4 8*",	// LAN 1 + LAN 3 + LAN 4
					"1 3 4 8*",	// LAN 1 + LAN 2 + LAN 4
					"2 3 4 8*",	// LAN 1 + LAN 2 + LAN 3
					"3 4 8*"};	// LAN 1 + LAN 2
			/* Set WAN ports */
			char *vlan2ports[] = {
					"0 8",		// Defaults
					"0 4 8",	// WAN + LAN 1
					"0 3 8",	// WAN + LAN 2
					"0 2 8",	// WAN + LAN 3
					"0 1 8",	// WAN + LAN 4
					"0 1 2 8"};	// WAN + LAN 3 + LAN 4
			nvram_set("vlan1ports", vlan1ports[stbport]);
			nvram_set("vlan2ports", vlan2ports[stbport]);
			break;
			}
		case MDL_RTN15U:
			{
			/* Set LAN ports */
			char *vlan1ports[] = {
					"0 1 2 3 8*",	// Defaults
					"0 1 2 8*",	// LAN 2 + LAN 3 + LAN 4
					"0 1 3 8*",	// LAN 1 + LAN 3 + LAN 4
					"0 2 3 8*",	// LAN 1 + LAN 2 + LAN 4
					"1 2 3 8*",	// LAN 1 + LAN 2 + LAN 3
					"2 3 8*"};	// LAN 1 + LAN 2
			/* Set WAN ports */
			char *vlan2ports[] = {
					"4 8",		// Defaults
					"3 4 8",	// WAN + LAN 1
					"2 4 8",	// WAN + LAN 2
					"1 4 8",	// WAN + LAN 3
					"0 4 8",	// WAN + LAN 4
					"0 1 4 8"};	// WAN + LAN 3 + LAN 4
			nvram_set("vlan1ports", vlan1ports[stbport]);
			nvram_set("vlan2ports", vlan2ports[stbport]);
			break;
			}
		case MDL_RTN12:
		case MDL_RTN12B1:
		case MDL_RTN12C1:
		case MDL_RTN10:
		case MDL_WL500GPV2:
			{
			/* Set LAN ports */
			char *vlan0ports[] = {
					"0 1 2 3 5*",	// Defaults
					"0 1 2 5*",	// LAN 2 + LAN 3 + LAN 4
					"0 1 3 5*",	// LAN 1 + LAN 3 + LAN 4
					"0 2 3 5*",	// LAN 1 + LAN 2 + LAN 4
					"1 2 3 5*",	// LAN 1 + LAN 2 + LAN 3
					"2 3 5*"};	// LAN 1 + LAN 2
			/* Set WAN ports */
			char *vlan1ports[] = {
					"4 5",		// Defaults
					"3 4 5",	// WAN + LAN 1
					"2 4 5",	// WAN + LAN 2
					"1 4 5",	// WAN + LAN 3
					"0 4 5",	// WAN + LAN 4
					"0 1 4 5"};	// WAN + LAN 3 + LAN 4
			nvram_set("vlan0ports", vlan0ports[stbport]);
			nvram_set("vlan1ports", vlan1ports[stbport]);
			break;
			}
		case MDL_WL330GE:
		case MDL_WL550GE:
		case MDL_WL520GU:
		case MDL_WL500GP:
		case MDL_DIR320:
			{
			/* Set LAN ports */
			char *vlan0ports[] = {
					"1 2 3 4 5*",	// Defaults
					"2 3 4 5*",	// LAN 2 + LAN 3 + LAN 4
					"1 3 4 5*",	// LAN 1 + LAN 3 + LAN 4
					"1 2 4 5*",	// LAN 1 + LAN 2 + LAN 4
					"1 2 3 5*",	// LAN 1 + LAN 2 + LAN 3
					"1 2 5*"};	// LAN 1 + LAN 2
			/* Set WAN ports */
			char *vlan1ports[] = {
					"0 5",		// Defaults
					"0 1 5",	// WAN + LAN 1
					"0 2 5",	// WAN + LAN 2
					"0 3 5",	// WAN + LAN 3
					"0 4 5",	// WAN + LAN 4
					"0 3 4 5"};	// WAN + LAN 3 + LAN4
			nvram_set("vlan0ports", vlan0ports[stbport]);
			nvram_set("vlan1ports", vlan1ports[stbport]);
			break;
			}
		case MDL_RTN10U:
			{
			/* Set LAN ports */
			char *vlan0ports[] = {
					"1 2 3 4 5*",	// Defaults
					"1 2 3 5*",	// LAN 2 + LAN 3 + LAN 4
					"1 2 4 5*",	// LAN 1 + LAN 3 + LAN 4
					"1 3 4 5*",	// LAN 1 + LAN 2 + LAN 4
					"2 3 4 5*",	// LAN 1 + LAN 2 + LAN 3
					"3 4 5*"};	// LAN 1 + LAN 2
			/* Set WAN ports */
			char *vlan1ports[] = {
					"0 5",		// Defaults
					"0 4 5",	// WAN + LAN 1
					"0 3 5",	// WAN + LAN 2
					"0 2 5",	// WAN + LAN 3
					"0 1 5",	// WAN + LAN 4
					"0 1 2 5"};	// WAN + LAN 3 + LAN4
			nvram_set("vlan0ports", vlan0ports[stbport]);
			nvram_set("vlan1ports", vlan1ports[stbport]);
			break;
			}
	}
	if (stbport == 0)
		nvram_unset("vlan_set_x");
	else
		nvram_set("vlan_set_x", "1");
}

static void
early_defaults(void)
{
	/* wl700ge -- boardflags are not set correctly */
	if (nvram_match("boardflags", "0x10") && nvram_get("default_boardflags")) 
	{
		nvram_set("boardflags", nvram_get("default_boardflags"));
	}

	if (nvram_match("wan_route_x", "IP_Bridged"))
	{
		switch (router_model)
		{
		    case MDL_RTN16:
		    case MDL_RTN15U:
		    case MDL_WNR3500L:
                        nvram_set("vlan1ports", "0 1 2 3 4 8*");
                        nvram_set("vlan2ports", "8");
                        break;
                    default:
			nvram_set("vlan0ports", "0 1 2 3 4 5*");
			nvram_set("vlan1ports", "5");
		}
		nvram_set("vlan_set_x", "1");
	}
	else { /* router mode, use vlans */

		/* bcm95350rg -- use vlans (wl550ge, wl500gp, wl700ge) vs wl320g - no vlans */
		if (nvram_match("wandevs", "et0") && 	/* ... wl500gpv2 */
		    (nvram_match("vlan1ports", "0 5u") || nvram_match("vlan1ports", "4 5u")) &&
		    (strtoul(nvram_safe_get("boardflags"), NULL, 0) & BFL_ENETVLAN) != 0)
		{
			nvram_set("wandevs", "vlan1");
			nvram_set("wan_ifname", "vlan1");
			nvram_set("wan_ifnames", "vlan1");
		
			/* data should be tagged for WAN port too */
			nvram_set("vlan1ports",
				nvram_match("vlan1ports", "0 5u") ? "0 5" : "4 5");
		}

		/* fix RT-N16 / RT-N15U vlans */
		if (router_model == MDL_RTN16 || router_model == MDL_RTN15U ||
		    router_model == MDL_WNR3500L)
		{
			if (!nvram_get("wan_ifname") || !nvram_get("vlan2hwname"))
			{
				nvram_unset("br0_ifnames");
				nvram_unset("vlan0ports");
				nvram_unset("vlan0hwname");
				if (router_model == MDL_RTN15U) {
					nvram_set("vlan1ports", "0 1 2 3 8*");
					nvram_set("vlan2ports", "4 8");
				} else {
					nvram_set("vlan1ports", "1 2 3 4 8*");
					nvram_set("vlan2ports", "0 8");
				}
				nvram_set("vlan1hwname", "et0");
				nvram_set("vlan2hwname", "et0");
				nvram_set("landevs", "vlan1 wl0");
				nvram_set("lan_ifnames", "vlan1 eth1");
				nvram_set("wandevs", "vlan2");
				nvram_set("wan_ifname", "vlan2");
				nvram_set("wan_ifnames", "vlan2");
				nvram_set("wan0_ifname", "vlan2");
				nvram_set("wan0_ifnames", "vlan2");
				nvram_set("wlan_ifname", "eth1");
			}
		}

		/* fix RT-N12 / RT-N12B1 / RT-N12C1 / RT-N10 / RT-N10U vlans */
		if (router_model == MDL_RTN12 || router_model == MDL_RTN12B1 || router_model == MDL_RTN12C1 ||
		    router_model == MDL_RTN10 || router_model == MDL_RTN10U)
		{
			if (!nvram_get("wan_ifname") || !nvram_get("vlan1hwname"))
			{
				if (router_model == MDL_RTN10U)
				{
					nvram_set("vlan0ports", "1 2 3 4 5*");
					nvram_set("vlan1ports", "0 5");
				} else {
					nvram_set("vlan0ports", "0 1 2 3 5*");
					nvram_set("vlan1ports", "4 5");
				}
				nvram_set("vlan0hwname", "et0");
				nvram_set("vlan1hwname", "et0");
				nvram_set("landevs", "vlan0 wl0");
				nvram_set("lan_ifnames", "vlan0 eth1");
				nvram_set("wandevs", "vlan1");
				nvram_set("wan_ifname", "vlan1");
				nvram_set("wan_ifnames", "vlan1");
				nvram_set("wan0_ifname", "vlan1");
				nvram_set("wan0_ifnames", "vlan1");
				nvram_set("wlan_ifname", "eth1");
			}
		}

		/* fix DLINK DIR-320 vlans */
		if (router_model == MDL_DIR320 && !nvram_get("wan_ifname"))
		{
			nvram_unset("vlan2ports");
			nvram_unset("vlan2hwname");
			nvram_set("vlan1hwname", "et0");
			nvram_set("vlan1ports", "0 5");
			nvram_set("wandevs", "vlan1");
			nvram_set("wan_ifname", "vlan1");
			nvram_set("wan_ifnames", "vlan1");
			nvram_set("wan0_ifname", "vlan1");
			nvram_set("wan0_ifnames", "vlan1");
		}

		/* STB port setting */
		stb_set();

	} // vlans

	/* fix RT-N15U AIR led */
	if (router_model == MDL_RTN15U && nvram_match("sb/1/ledbh0", "2"))
		nvram_set("sb/1/ledbh0", "8");

	/* wl550ge -- missing wl0gpio values */
	if ((router_model == MDL_WL320GE || router_model == MDL_WL550GE) &&
	    !nvram_get("wl0gpio0"))
	{
		nvram_set("wl0gpio0", "2");
		nvram_set("wl0gpio1", "0");
		nvram_set("wl0gpio2", "0");
		nvram_set("wl0gpio3", "0");
	}
	
	/* fix AIR LED -- override default SROM setting and fix wl550ge config */
	if (!nvram_get("wl0gpio0") || nvram_match("wl0gpio0", "2"))
		nvram_set("wl0gpio0", "0x88");
	
	/* WL520gu/gc/gp/330ge */
	if (nvram_match("wl0gpio1", "0x02"))
		nvram_set("wl0gpio1", "0x88");

	/* wl500gp -- 16mb memory activated, 32 available */
	if (router_model == MDL_WL500GP &&
	    nvram_match("sdram_init", "0x000b") && nvram_match("sdram_config", "0x0062"))
	{
		nvram_set("sdram_init", "0x0009");
		nvram_set("sdram_ncdl", "0");
	}

	/* fix DIR-320 gpio */
	if (router_model == MDL_DIR320 && nvram_match("wl0gpio0", "255"))
	{
		nvram_set("wl0gpio0", "8");
		nvram_set("wl0gpio1", "0");
		nvram_set("wl0gpio2", "0");
		nvram_set("wl0gpio3", "0");
	}

	/* fix WL500W mac adresses for WAN port */
	if (nvram_match("et1macaddr", "00:90:4c:a1:00:2d")) 
		nvram_set("et1macaddr", nvram_get("et0macaddr"));

#if 0	/* leave it commented out until VLANs will work */
	/* WL500W could have vlans enabled */
	if (router_model == MDL_WL500W)
	{
		if (strtoul(nvram_safe_get("boardflags"), NULL, 0) & BFL_ENETVLAN)
		{
			if (!nvram_get("vlan0ports")) {
				nvram_set("vlan0hwname", "et0");
				nvram_set("vlan0ports", "0 1 2 3 5*");
				nvram_set("vlan1hwname", "et0");
				nvram_set("vlan1ports", "4 5");
			}
			nvram_set("lan_ifnames", "vlan0 eth2");
			nvram_set("wan_ifname", "vlan1");
		} else {
			nvram_set("lan_ifnames", "eth0 eth2");
			nvram_set("wan_ifname", "eth1");
		}
		nvram_set("lan_ifname", "br0");
		nvram_set("wan_ifnames", nvram_get("wan_ifname"));
	}
#endif

	/* set lan_hostname */
	if (!nvram_invmatch("lan_hostname", ""))
	{
		/* derive from et0 mac addr */
		char *mac = nvram_get("et0macaddr");
		if (mac && strlen(mac) == 17)
		{
			char hostname[16];
			sprintf(hostname, "WL-%c%c%c%c%c%c%c%c%c%c%c%c",
				mac[0], mac[1], mac[3], mac[4], mac[6], mac[7],
				mac[9], mac[10], mac[12], mac[13], mac[15], mac[16]);
			nvram_set("lan_hostname", hostname);
		}
	}
}

static void
restore_defaults(void)
{
	struct nvram_tuple generic[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "eth0 eth2 eth3 eth4", 0 },
		{ "wan_ifname", "eth1", 0 },
		{ "wan_ifnames", "eth1", 0 },
		{ 0, 0, 0 }
	};
	struct nvram_tuple vlan[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "vlan0 eth1 eth2 eth3", 0 },
		{ "wan_ifname", "vlan1", 0 },
		{ "wan_ifnames", "vlan1", 0 },
		{ 0, 0, 0 }
	};
	struct nvram_tuple dyna[] = {
		{ "lan_ifname", "br0", 0 },
		{ "lan_ifnames", "", 0 },
		{ "wan_ifname", "", 0 },
		{ "wan_ifnames", "", 0 },
		/* default with vlans disabled */
		{ "wan_nat_x", "0", 0},
		{ "wan_route_x", "IP_Bridged", 0},
		{ 0, 0, 0 }
	};


	struct nvram_tuple *linux_overrides;
	struct nvram_tuple *t, *u;
	int restore_defaults, i;
	uint boardflags;
	char *landevs, *wandevs;
	char lan_ifnames[128], wan_ifnames[128];
	char wan_ifname[32], *next;
	int len;
	int ap = 0;

	/* Restore defaults if told to or OS has changed */
	restore_defaults = !nvram_match("restore_defaults", "0") || nvram_invmatch("os_name", "linux");

	if (restore_defaults)
		cprintf("Restoring defaults...");

	/* Delete dynamically generated variables */
	if (restore_defaults) {
		char tmp[100], prefix[sizeof("wlXXXXXXXXXX_")];
		for (i = 0; i < MAX_NVPARSE; i++) {
			del_forward_port(i);
			del_autofw_port(i);
			snprintf(prefix, sizeof(prefix), "wl%d_", i);
			for (t = router_defaults; t->name; t ++) {
				if (!strncmp(t->name, "wl_", 3))
					nvram_unset(strcat_r(prefix, &t->name[3], tmp));
			}
			snprintf(prefix, sizeof(prefix), "wan%d_", i);
			for (t = router_defaults; t->name; t ++) {
				if (!strncmp(t->name, "wan_", 4))
					nvram_unset(strcat_r(prefix, &t->name[4], tmp));
			}
		}
	}

	/* 
	 * Build bridged i/f name list and wan i/f name list from lan device name list
	 * and wan device name list. Both lan device list "landevs" and wan device list
	 * "wandevs" must exist in order to preceed.
	 */
	if ((landevs = nvram_get("landevs")) && (wandevs = nvram_get("wandevs"))) {
		/* build bridged i/f list based on nvram variable "landevs" */
		len = sizeof(lan_ifnames);
		if (!build_ifnames(landevs, lan_ifnames, &len) && len)
			dyna[1].value = lan_ifnames;
		else
			goto canned_config;
		/* build wan i/f list based on nvram variable "wandevs" */
		len = sizeof(wan_ifnames);
		if (!build_ifnames(wandevs, wan_ifnames, &len) && len) {
			dyna[3].value = wan_ifnames;
			foreach (wan_ifname, wan_ifnames, next) {
				dyna[2].value = wan_ifname;
				break;
			}
		}
		else
			ap = 1;
		
		/* if vlans enabled -- enable router mode */
		if ((strtoul(nvram_safe_get("boardflags"), 
			NULL, 0) & BFL_ENETVLAN) != 0) dyna[4].name = NULL;

		linux_overrides = dyna;
	}
	/* override lan i/f name list and wan i/f name list with default values */
	else {
canned_config:
		boardflags = strtoul(nvram_safe_get("boardflags"), NULL, 0);
		if (boardflags & BFL_ENETVLAN)
			linux_overrides = vlan;
		else
			linux_overrides = generic;
	}
	
	/* Restore defaults */
	for (t = router_defaults; t->name; t++) {
		if (restore_defaults || !nvram_get(t->name)) {
			for (u = linux_overrides; u && u->name; u++) {
				if (!strcmp(t->name, u->name)) {
					nvram_set(u->name, u->value);
					break;
				}
			}
			if (!u || !u->name)
				nvram_set(t->name, t->value);
		}
	}

	/* Force to AP */
	if (ap)
		nvram_set("router_disable", "1");

	/* Always set OS defaults */
	nvram_set("os_name", "linux");
	nvram_set("os_version", EPI_VERSION_STR);
	nvram_set("os_date", __DATE__);

	nvram_set("is_modified", "0");

	/* Commit values */
	if (restore_defaults) {
		/* default value of vlan */
		nvram_commit();		
		cprintf("done\n");
	}
}

static void
set_wan0_vars(void)
{
	int unit;
	char tmp[100], prefix[WAN_PREFIX_SZ];
	
	/* check if there are any connections configured */
	for (unit = 0; unit < MAX_NVPARSE; unit ++) {
		snprintf(prefix, sizeof(prefix), "wan%d_", unit);
		if (nvram_get(strcat_r(prefix, "unit", tmp)))
			break;
	}
	/* automatically configure wan0_ if no connections found */
	if (unit >= MAX_NVPARSE) {
		struct nvram_tuple *t;
		char *v;

		/* Write through to wan0_ variable set */
		snprintf(prefix, sizeof(prefix), "wan%d_", 0);
		for (t = router_defaults; t->name; t ++) {
			if (!strncmp(t->name, "wan_", 4)) {
				if (nvram_get(strcat_r(prefix, &t->name[4], tmp)))
					continue;
				v = nvram_get(t->name);
				nvram_set(tmp, v ? v : t->value);
			}
		}
		nvram_set(strcat_r(prefix, "unit", tmp), "0");
		nvram_set(strcat_r(prefix, "desc", tmp), "Default Connection");
		nvram_set(strcat_r(prefix, "primary", tmp), "1");
	}
	/* reconfigure wan0 for client mode */
	if (nvram_invmatch("wl_mode_ex", "ap")) {
		nvram_set("wan0_ifname", nvram_safe_get("wl0_ifname"));
		nvram_set("wan0_ifnames", nvram_safe_get("wl0_ifname"));
	} else {
		nvram_set("wan0_ifname", nvram_get("wan_ifname"));
		nvram_set("wan0_ifnames", nvram_get("wan_ifnames"));
	}
	nvram_set("wan0_priority", "0");
}


/* States */
enum RC_STATES {
	RESTART,
	STOP,
	START,
	TIMER,
	IDLE,
	SERVICE,
};
static int rc_state = START;
static int signalled = -1;


/* Signal handling */
static void rc_signal(int sig)
{
	if (rc_state == IDLE) {
		switch (sig) {
			case SIGHUP:
				dprintf("signalling RESTART\n");
				signalled = RESTART;
				break;
			case SIGUSR2:
				dprintf("signalling START\n");
				signalled = START;
				break;
			case SIGINT:
				dprintf("signalling STOP\n");
				signalled = STOP;
				break;
			case SIGALRM:
				dprintf("signalling TIMER\n");
				signalled = TIMER;
				break;
			case SIGUSR1:
				dprintf("signalling USR1\n");
				signalled = SERVICE;
				break;
		}
	}
}

/* Timer procedure */
static int do_timer(void)
{
#ifndef ASUS_EXT
	int interval = nvram_get_int("timer_interval");

	dprintf("%d\n", interval);

	if (interval == 0)
		return 0;

	/* Sync time */
	start_ntpc();
#endif
	update_tztime(0);
#ifndef ASUS_EXT
	alarm(interval);
#endif
	return 0;
}

/* Main loop */
static void
main_loop(void)
{
	sigset_t sigset;
	pid_t shell_pid = 0;
	uint boardflags;

	/* Convert vital config before loading modules */
	early_defaults();

	/* Basic initialization */
	sysinit();

	/* Setup signal handlers */
	signal_init();
	signal(SIGHUP, rc_signal);
	signal(SIGUSR1, rc_signal);
	signal(SIGUSR2, rc_signal);
	signal(SIGINT, rc_signal);
	signal(SIGALRM, rc_signal);
	sigemptyset(&sigset);

	/* Give user a chance to run a shell before bringing up the rest of the system */
	if (!noconsole)
		run_shell(1, 0);

	/* Add vlan */
	boardflags = strtoul(nvram_safe_get("boardflags"), NULL, 0);

	/* Add loopback */
	config_loopback();

	/* Convert deprecated variables */
	convert_deprecated();

	/* Restore defaults if necessary */
	restore_defaults();

#ifdef ASUS_EXT	
	convert_asus_values();
#endif

	/* Setup wan0 variables if necessary */
	set_wan0_vars();
	
	/* Loop forever */
	for (;;) {
		switch (rc_state) {
		case SERVICE:
			dprintf("SERVICE\n");
			service_handle();
			rc_state = IDLE;
			break;
		case RESTART:
			dprintf("RESTART\n");
			/* Fall through */
		case STOP:
			dprintf("STOP\n");

#ifdef ASUS_EXT
			stop_misc();
#endif
			stop_services();
			stop_wan();
			stop_lan();

			if (boardflags & BFL_ENETVLAN)
				stop_vlan();

			if (rc_state == STOP) {
				rc_state = IDLE;
				break;
			}
			/* Fall through */
		case START:
			dprintf("START\n");

			if (boardflags & BFL_ENETVLAN)
				start_vlan();

			start_lan();
			//if (restore_defaults_g) 
			//{
			//	goto retry;
			//}
			start_services();
			start_wan();
#ifndef __CONFIG_BCMWL5__
			start_nas("wan");
#endif

#ifdef ASUS_EXT
			start_misc();
#endif
			eval("/usr/local/sbin/post-boot");
#ifdef ASUS_EXT
			sleep(1);
#endif
			/* Fall through */
		case TIMER:
			dprintf("TIMER\n");
			do_timer();			
			/* Fall through */
		case IDLE:
			dprintf("IDLE\n");
			rc_state = IDLE;
			/* Wait for user input or state change */
			while (signalled == -1) {
				if (!noconsole && (!shell_pid || kill(shell_pid, 0) != 0))
					shell_pid = run_shell(0, 1);
				else
					sigsuspend(&sigset);
			}
			rc_state = signalled;
			signalled = -1;
			break;
		default:
			dprintf("UNKNOWN\n");
			return;
		}
	}
}

static int
check_option(int argc, char * const argv[], int *index, const char *option) {
	int res;
	int found = 0;
	opterr = 0;

	if (strlen(option) == 0)
		return 0;

	while ((res = getopt(argc, argv, option)) != -1) {
		if (!found)
			found = ((char)res == option[0]);
	}
	*index = optind;
	return found;
}

int
main(int argc, char **argv)
{
	char *base = strrchr(argv[0], '/');
	
	base = base ? base + 1 : argv[0];

	router_model = get_model();

	/* init */
	if (!strcmp(base, "init")) {
		main_loop();
		return 0;
	}

	/* Set TZ for all rc programs */
	setenv_tz();

#ifdef DEBUG
	cprintf("rc applet: %s %s %s\n",
		 base, (argc>1) ? argv[1] : "", (argc>2) ? argv[2] : "");
#endif

	/* ppp */
	if (!strcmp(base, "ip-up"))
		return ipup_main(argc, argv);
	else if (!strcmp(base, "ip-down"))
		return ipdown_main(argc, argv);
	else if (!strcmp(base, "ip-pre-up"))
		return ippreup_main(argc, argv);
#ifdef __CONFIG_IPV6__
	else if (!strcmp(base, "ipv6-up"))
		return ip6up_main(argc, argv);
	else if (!strcmp(base, "ipv6-down"))
		return ip6down_main(argc, argv);
#endif
#ifdef PPPD_AUTH_UNUSED
	else if (!strcmp(base, "auth-up"))
		return authup_main(argc, argv);
	else if (!strcmp(base, "auth-down"))
		return authdown_main(argc, argv);
	else if (!strcmp(base, "auth-fail"))
		return authfail_main(argc, argv);
#endif
	/* udhcpc.script [ deconfig bound renew ] */
	else if (!strcmp(base, "udhcpc.script"))
		return udhcpc_main(argc, argv);
	else if (!strcmp(base, "zcip.script"))
		return zcip_main(argc, argv);
#ifdef __CONFIG_EAPOL__
	else if (!strcmp(base, "wpa_cli.script"))
		return wpacli_main(argc, argv);
#endif
#ifdef __CONFIG_IPV6__
	else if (!strcmp(base, "dhcp6c.script"))
		return dhcp6c_main(argc, argv);
#endif
#ifdef __CONFIG_MADWIMAX__
	/* madwimax [ if-create if-up if-down if-release ] */
	else if (!strcmp(base, "madwimax.events"))
		return madwimax_main(argc, argv);
#endif
#ifdef __CONFIG_MODEM__
	/* lsmodem [-s|-j]*/
	else if (!strcmp(base, "lsmodem"))
		return lsmodem_main(argc, argv);
#endif
	/* restore default */
	else if (!strcmp(base, "restore"))
	{
		if (argc == 2)
		{
			int step = atoi(argv[1]);
			if (step >= 1) {
				nvram_set("vlan_enable", "1");
				restore_defaults();
			}
			/* Setup wan0 variables if necessary */
			if (step >= 2)
				set_wan0_vars();
			if (step >= 3)
				start_vlan();
			if (step >= 4)
				start_lan();
		}
		return 0;
	}

	/* erase [device] */
	else if (!strcmp(base, "erase")) {
		if (argc == 2) {
			return mtd_erase(argv[1]);
		} else {
			fprintf(stderr, "usage: erase [device]\n");
			return EINVAL;
		}
	}

	/* write [path] [device] */
	else if (!strcmp(base, "write")) {
		int index;
		int reboot = check_option(argc, argv, &index, "r");
		if (argc >= index+2) {
			if (reboot) preshutdown_system();
			int res = mtd_write(argv[index], argv[index+1]);
			if (reboot && !res) kill(1, SIGABRT);
			return res;
		} else {
			fprintf(stderr, "usage: write [-r] [path] [device]\n"
							"		-r: reboot after write\n");
			return EINVAL;
		}
	}

	/* flash [path] [device] */
	else if (!strcmp(base, "flash")) {
		int index;
		int reboot = check_option(argc, argv, &index, "r");
		if (argc >= index+2) {
			if (reboot) preshutdown_system();
			int res = mtd_flash(argv[index], argv[index+1]);
			if (reboot && !res) kill(1, SIGABRT);
			return res;
		} else {
			fprintf(stderr, "usage: flash [-r] [path] [device]\n"
							"		-r: reboot after flash\n");
			return EINVAL;
		}
	}

	/* hotplug [event] */
	else if (!strcmp(base, "hotplug")) {
		if (argc >= 2) {
			eval("/sbin/mdev");
		#if defined(__CONFIG_MODEM__) && defined(RC_SEMAPHORE_ENABLED)
			if (!strcmp(argv[1], "usb-serial"))
				return usb_communication_device_processcheck(1);
		#endif
			if (!strcmp(argv[1], "net"))
				return hotplug_net();
#ifdef ASUS_EXT
			else if (!strcmp(argv[1], "usb"))
				return hotplug_usb();
#endif
		} else {
			fprintf(stderr, "usage: hotplug [event]\n");
			return EINVAL;
		}
	}


#ifdef ASUS_EXT
	/* ddns update ok */
	else if (!strcmp(base, "stopservice")) {
		return stop_service_main();
	}
	/* ddns update ok */
	else if (!strcmp(base, "ddns_updated")) 
	{
		return ddns_updated_main();
	}
	/* ddns update ok */
	else if (!strcmp(base, "start_ddns")) 
	{
		int type = 0;
		if (argc >= 2)
			type = atoi(argv[1]);
		return start_ddns(NULL, type);
	}
	/* send alarm */
	else if (!strcmp(base, "sendalarm")) {
		return sendalarm_main(argc, argv);
	}
	/* invoke watchdog */
	else if (!strcmp(base, "watchdog")) {
		return watchdog_main();
	}
	/* run ntp client */
	else if (!strcmp(base, "ntp")) {
		return !start_ntpc();
	}
#ifdef USB_SUPPORT
#ifdef __CONFIG_RCAMD__
	/* remove webcam module */
	else if (!strcmp(base, "rmwebcam")) {
		if (argc >= 2)
			return (remove_usb_webcam(nvram_safe_get("usb_web_device")));
		else return EINVAL;
	}
	/* run rcamd */
	else if (!strcmp(base, "rcamdmain")) {
		return (hotplug_usb_webcam(nvram_safe_get("usb_web_device")));
	}
#endif
	/* remove usbstorage module */
	else if (!strcmp(base, "rmstorage")) {
		int scsi_host = -1;
		if (argc >= 2)
			scsi_host = atoi(argv[1]);
		return remove_usb_mass(NULL, scsi_host);
	}
	/* run ftp server */
	else if (!strcmp(base, "start_ftpd")) {
		return restart_ftpd();
	}
	/* run samba server */
	else if (!strcmp(base, "start_smbd")) {
		return restart_smbd();
	}
#endif //USB_SUPPORT
	/* write srom */
	else if (!strcmp(base, "wsrom")) 
	{
		do_timer();
		if (argc >= 4) 
			return wsrom_main(argv[1], atoi(argv[2]), atoi(argv[3]));
		else {
			fprintf(stderr, "usage: wsrom [dev] [position] [value in 2 bytes]\n");
			return EINVAL;
		}
	}
	/* write srom */
	else if (!strcmp(base, "rsrom")) 
	{
		if (argc >= 3)
		{	 
			rsrom_main(argv[1], atoi(argv[2]), 1);
			return 0;
		}
		else {
			fprintf(stderr, "usage: rsrom [dev] [position]\n");
			return EINVAL;
		}
	}
	/* write mac */
	else if (!strcmp(base, "wmac")) 
	{
		if (argc >= 3) 
			return write_mac(argv[1], argv[2]);
		else {
			fprintf(stderr, "usage: wmac [dev] [mac]\n");
			return EINVAL;
		}
	}
	/* wlan update */
	else if (!strcmp(base, "wlan_update")) 
	{
		wlan_update();
		return 0;
	}
	/* landhcpc [ deconfig bound renew ], for lan only */
	else if (!strcmp(base, "landhcpc"))
		return udhcpc_ex_main(argc, argv);
	else if (!strcmp(base, "bpa_connect"))
                 return bpa_connect_main(argc, argv);
        else if (!strcmp(base, "bpa_disconnect"))
                 return bpa_disconnect_main(argc, argv);
#endif

	/* rc [stop|start|restart ] */
	else if (!strcmp(base, "rc")) {
		if (argc == 2) {
			if (strncmp(argv[1], "start", 5) == 0)
				return kill(1, SIGUSR2);
			else if (strncmp(argv[1], "stop", 4) == 0)
				return kill(1, SIGINT);
			else if (strncmp(argv[1], "restart", 7) == 0)
				return kill(1, SIGHUP);
		} else {
			fprintf(stderr, "usage: rc [start|stop|restart]\n");
			return EINVAL;
		}
	} else if (!strcmp(base, "halt")) {
		kill(1, SIGQUIT);
	} else if (!strcmp(base, "reboot")) {
		kill(1, SIGTERM);
	} else if (!strcmp(base, "poweron")) {
		return poweron_main(argc, argv);
	}
	
	return EINVAL;
}
