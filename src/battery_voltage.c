/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>

/* Strict decimal protocol: 0..99.99 volts, at most two decimal places. */
int battery_voltage_parse(const char *text, unsigned int *centivolts)
{
	unsigned int whole = 0, fraction = 0, digits = 0;
	if (*text < '0' || *text > '9') {
		return -1;
	}
	while (*text >= '0' && *text <= '9') {
		if (++digits > 2) {
			return -1;
		}
		whole = whole * 10 + (*text++ - '0');
	}
	if (*text == '.') {
		text++;
		if (*text < '0' || *text > '9') {
			return -1;
		}
		fraction = (*text++ - '0') * 10;
		if (*text >= '0' && *text <= '9') {
			fraction += *text++ - '0';
		}
	}
	if (*text != '\0') {
		return -1;
	}
	*centivolts = whole * 100 + fraction;
	return 0;
}
