/* See LICENSE file for copyright and license details. */
#include <ifaddrs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../util.h"

#if defined(__linux__)
	#include <limits.h>
	#include <linux/wireless.h>

	const char *
	wifi_perc(const char *interface)
	{
		int cur;
		size_t i;
		char *p, *datastart;
		char path[PATH_MAX];
		char status[5];
		FILE *fp;

		if (esnprintf(path, sizeof(path), "/sys/class/net/%s/operstate",
		              interface) < 0) {
			return NULL;
		}
		if (!(fp = fopen(path, "r"))) {
			warn("fopen '%s':", path);
			return NULL;
		}
		p = fgets(status, 5, fp);
		fclose(fp);
		if (!p || strcmp(status, "up\n") != 0) {
			return NULL;
		}

		if (!(fp = fopen("/proc/net/wireless", "r"))) {
			warn("fopen '/proc/net/wireless':");
			return NULL;
		}

		for (i = 0; i < 3; i++) {
			if (!(p = fgets(buf, sizeof(buf) - 1, fp)))
				break;
		}
		fclose(fp);
		if (i < 2 || !p) {
			return NULL;
		}

		if (!(datastart = strstr(buf, interface))) {
			return NULL;
		}

		datastart = (datastart+(strlen(interface)+1));
		sscanf(datastart + 1, " %*d   %d  %*d  %*d\t\t  %*d\t   "
		       "%*d\t\t%*d\t\t %*d\t  %*d\t\t %*d", &cur);

		/* 70 is the max of /proc/net/wireless */
		return bprintf("%d", (int)((float)cur / 70 * 100));
	}

	const char *
	wifi_essid(const char *interface)
	{
		static char id[IW_ESSID_MAX_SIZE+1];
		int sockfd;
		struct iwreq wreq;

		memset(&wreq, 0, sizeof(struct iwreq));
		wreq.u.essid.length = IW_ESSID_MAX_SIZE+1;
		if (esnprintf(wreq.ifr_name, sizeof(wreq.ifr_name), "%s",
		              interface) < 0) {
			return NULL;
		}

		if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			warn("socket 'AF_INET':");
			return NULL;
		}
		wreq.u.essid.pointer = id;
		if (ioctl(sockfd,SIOCGIWESSID, &wreq) < 0) {
			warn("ioctl 'SIOCGIWESSID':");
			close(sockfd);
			return NULL;
		}

		close(sockfd);

		if (!strcmp(id, "")) {
			return NULL;
		}

		return id;
	}
#elif defined(__OpenBSD__) 
	#include <net/if.h>
	#include <net/if_media.h>
	#include <net80211/ieee80211.h>
	#include <sys/select.h> /* before <sys/ieee80211_ioctl.h> for NBBY */
	#include <net80211/ieee80211_ioctl.h>
	#include <stdlib.h>
	#include <sys/types.h>

	static int
	load_ieee80211_nodereq(const char *interface, struct ieee80211_nodereq *nr)
	{
		struct ieee80211_bssid bssid;
		int sockfd;
		uint8_t zero_bssid[IEEE80211_ADDR_LEN];

		memset(&bssid, 0, sizeof(bssid));
		memset(nr, 0, sizeof(struct ieee80211_nodereq));
		if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			warn("socket 'AF_INET':");
			return 0;
		}
		strlcpy(bssid.i_name, interface, sizeof(bssid.i_name));
		if ((ioctl(sockfd, SIOCG80211BSSID, &bssid)) < 0) {
			warn("ioctl 'SIOCG80211BSSID':");
			close(sockfd);
			return 0;
		}
		memset(&zero_bssid, 0, sizeof(zero_bssid));
		if (memcmp(bssid.i_bssid, zero_bssid,
		    IEEE80211_ADDR_LEN) == 0) {
			close(sockfd);
			return 0;
		}
		strlcpy(nr->nr_ifname, interface, sizeof(nr->nr_ifname));
		memcpy(&nr->nr_macaddr, bssid.i_bssid, sizeof(nr->nr_macaddr));
		if ((ioctl(sockfd, SIOCG80211NODE, nr)) < 0 && nr->nr_rssi) {
			warn("ioctl 'SIOCG80211NODE':");
			close(sockfd);
			return 0;
		}

		return close(sockfd), 1;
	}

	const char *
	wifi_perc(const char *interface)
	{
		struct ieee80211_nodereq nr;
		int q;

		if (load_ieee80211_nodereq(interface, &nr)) {
			if (nr.nr_max_rssi) {
				q = IEEE80211_NODEREQ_RSSI(&nr);
			} else {
				q = nr.nr_rssi >= -50 ? 100 :
				    (nr.nr_rssi <= -100 ? 0 :
				    (2 * (nr.nr_rssi + 100)));
			}
			return bprintf("%d", q);
		}

		return NULL;
	}

	const char *
	wifi_essid(const char *interface)
	{
		struct ieee80211_nodereq nr;

		if (load_ieee80211_nodereq(interface, &nr)) {
			return bprintf("%s", nr.nr_nwid);
		}

		return NULL;
	}
#elif defined(__FreeBSD__)
	#include <sys/ioctl.h>
	#include <net/ethernet.h>
	#include <net/if.h>
	#include <net80211/ieee80211_ioctl.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <stdlib.h>
	#include <dev/wi/if_wavelan_ieee.h>
	#include <net/if.h>
	#include <net/if_media.h>

	struct wi_req {
		u_int16_t	wi_len;
		u_int16_t	wi_type;
		u_int16_t	wi_val[WI_MAX_DATALEN];
	};	

	const char *
	wifi_perc(const char *interface)
	{
		int	s;
		struct ifreq ifr;
		struct wi_req wreq;

		if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
			return NULL;

		bzero((char *)&wreq, sizeof(wreq));
		bzero((char *)&ifr, sizeof(ifr));

		wreq.wi_len = WI_MAX_DATALEN;
		wreq.wi_type = WI_RID_COMMS_QUALITY;
		ifr.ifr_data = (caddr_t)&wreq;
		strlcpy(ifr.ifr_name, interface, sizeof(ifr.ifr_name));

		if (ioctl(s, SIOCGWAVELAN, &ifr) == -1) {
			close(s);
			return NULL;
		}
		close(s);

		return bprintf("%d", (wreq.wi_val[1]));
	}

	const char *
	wifi_essid(const char *interface)
	{
		struct ieee80211req ireq;
		char ssid[IEEE80211_NWID_LEN + 1];

		int sockfd;
		sockfd = socket(AF_INET, SOCK_DGRAM, 0);

		memset(&ireq, 0,sizeof(ireq));
		strncpy(ireq.i_name, interface, sizeof(ireq.i_name));

		ireq.i_type = IEEE80211_IOC_SSID;
		ireq.i_data = &ssid;
		ireq.i_len = sizeof(ssid);

		ioctl(sockfd, SIOCG80211, (caddr_t)&ireq);
		close(sockfd);
		int len;
		if (ireq.i_len < sizeof(ssid))
			len = ireq.i_len + 1;
		else
			len = sizeof(ssid);

		ssid[len - 1] = '\0';

		return bprintf("%s", ssid);
	}
#endif
