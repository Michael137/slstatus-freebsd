/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>

#include "../util.h"

#if defined(__linux__)
	#include <limits.h>
	#include <stdint.h>
	#include <unistd.h>

	static const char *
	pick(const char *bat, const char *f1, const char *f2, char *path,
	     size_t length)
	{
		if (esnprintf(path, length, f1, bat) > 0 &&
		    access(path, R_OK) == 0) {
			return f1;
		}

		if (esnprintf(path, length, f2, bat) > 0 &&
		    access(path, R_OK) == 0) {
			return f2;
		}

		return NULL;
	}

	const char *
	battery_perc(const char *bat)
	{
		int perc;
		char path[PATH_MAX];

		if (esnprintf(path, sizeof(path),
		              "/sys/class/power_supply/%s/capacity", bat) < 0) {
			return NULL;
		}
		if (pscanf(path, "%d", &perc) != 1) {
			return NULL;
		}

		return bprintf("%d", perc);
	}

	const char *
	battery_state(const char *bat)
	{
		static struct {
			char *state;
			char *symbol;
		} map[] = {
			{ "Charging",    "+" },
			{ "Discharging", "-" },
		};
		size_t i;
		char path[PATH_MAX], state[12];

		if (esnprintf(path, sizeof(path),
		              "/sys/class/power_supply/%s/status", bat) < 0) {
			return NULL;
		}
		if (pscanf(path, "%12s", state) != 1) {
			return NULL;
		}

		for (i = 0; i < LEN(map); i++) {
			if (!strcmp(map[i].state, state)) {
				break;
			}
		}
		return (i == LEN(map)) ? "?" : map[i].symbol;
	}

	const char *
	battery_remaining(const char *bat)
	{
		uintmax_t charge_now, current_now, m, h;
		double timeleft;
		char path[PATH_MAX], state[12];

		if (esnprintf(path, sizeof(path),
		              "/sys/class/power_supply/%s/status", bat) < 0) {
			return NULL;
		}
		if (pscanf(path, "%12s", state) != 1) {
			return NULL;
		}

		if (!pick(bat, "/sys/class/power_supply/%s/charge_now",
		          "/sys/class/power_supply/%s/energy_now", path,
		          sizeof(path)) ||
		    pscanf(path, "%ju", &charge_now) < 0) {
			return NULL;
		}

		if (!strcmp(state, "Discharging")) {
			if (!pick(bat, "/sys/class/power_supply/%s/current_now",
			          "/sys/class/power_supply/%s/power_now", path,
			          sizeof(path)) ||
			    pscanf(path, "%ju", &current_now) < 0) {
				return NULL;
			}

			if (current_now == 0) {
				return NULL;
			}

			timeleft = (double)charge_now / (double)current_now;
			h = timeleft;
			m = (timeleft - (double)h) * 60;

			return bprintf("%juh %jum", h, m);
		}

		return "";
	}
#elif defined(__OpenBSD__)
	#include <fcntl.h>
	#include <machine/apmvar.h>
	#include <sys/ioctl.h>
	#include <unistd.h>

	static int
	load_apm_power_info(struct apm_power_info *apm_info)
	{
		int fd;

		fd = open("/dev/apm", O_RDONLY);
		if (fd < 0) {
			warn("open '/dev/apm':");
			return 0;
		}

		memset(apm_info, 0, sizeof(struct apm_power_info));
		if (ioctl(fd, APM_IOC_GETPOWER, apm_info) < 0) {
			warn("ioctl 'APM_IOC_GETPOWER':");
			close(fd);
			return 0;
		}
		return close(fd), 1;
	}

	const char *
	battery_perc(const char *unused)
	{
		struct apm_power_info apm_info;

		if (load_apm_power_info(&apm_info)) {
			return bprintf("%d", apm_info.battery_life);
		}

		return NULL;
	}

	const char *
	battery_state(const char *unused)
	{
		struct {
			unsigned int state;
			char *symbol;
		} map[] = {
			{ APM_AC_ON,      "+" },
			{ APM_AC_OFF,     "-" },
		};
		struct apm_power_info apm_info;
		size_t i;

		if (load_apm_power_info(&apm_info)) {
			for (i = 0; i < LEN(map); i++) {
				if (map[i].state == apm_info.ac_state) {
					break;
				}
			}
			return (i == LEN(map)) ? "?" : map[i].symbol;
		}

		return NULL;
	}

	const char *
	battery_remaining(const char *unused)
	{
		struct apm_power_info apm_info;

		if (load_apm_power_info(&apm_info)) {
			if (apm_info.ac_state != APM_AC_ON) {
				return bprintf("%uh %02um",
			                       apm_info.minutes_left / 60,
				               apm_info.minutes_left % 60);
			} else {
				return "";
			}
		}

		return NULL;
	}
#elif defined(__FreeBSD__)
	#include <unistd.h>
	#include <sys/sysctl.h>
	#include <sys/ioctl.h>
	#include <dev/acpica/acpiio.h>
	#include <fcntl.h>

	int load_battery_info(union acpi_battery_ioctl_arg* battio) {
		int fd;

		fd = open("/dev/acpi", O_RDONLY);
		if (fd < 0) {
			warn("open '/dev/acpi':");
			return 0;
		}

		if (ioctl(fd, ACPIIO_BATT_GET_BATTINFO, battio) == -1
				|| (battio->battinfo).state == ACPI_BATT_STAT_NOT_PRESENT) {
			warn("ioctl 'ACPIIO_BATT_GET_BATTINFO':");
			close(fd);
			return 0;
		} 

		return close(fd), 1;
	}

	const char *
	battery_perc(const char *unused)
	{
		union acpi_battery_ioctl_arg battio;
		battio.unit = 0;
		if(load_battery_info(&battio) == 0
				|| battio.battinfo.cap == -1)
			return NULL;

		return bprintf("%d", battio.battinfo.cap);
	}

	const char *
	battery_state(const char *unused)
	{
		union acpi_battery_ioctl_arg battio;
		char const* state;

		battio.unit = 0;
		if(load_battery_info(&battio) == 0)
			return NULL;

		switch(battio.battinfo.state & ACPI_BATT_STAT_BST_MASK) {
			case 0:
			case ACPI_BATT_STAT_CHARGING:
			case ACPI_BATT_STAT_CHARGING | ACPI_BATT_STAT_CRITICAL:
				state = "+";
				break;
			case ACPI_BATT_STAT_DISCHARG:
			case ACPI_BATT_STAT_DISCHARG | ACPI_BATT_STAT_CRITICAL:
				state = "-";
				break;
			case ACPI_BATT_STAT_CRITICAL:
				state = "!";
				break;
			default:
				state = "?";
		}

		return state;
	}

	const char *
	battery_remaining(const char *unused)
	{
		int rem;
		union acpi_battery_ioctl_arg battio;

		battio.unit = 0;
		if(load_battery_info(&battio) == 0)
			return NULL;

		// In minutes
		rem = battio.battinfo.min;

		if(rem == -1)
			return NULL;

		return bprintf("%uh %02um", rem / 60, rem % 60);
	}
#endif
