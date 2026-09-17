/* Run: cc -Wall -Wextra -Werror tests/test_voltage.c src/battery_voltage.c -o /tmp/test_voltage && /tmp/test_voltage */
#include <assert.h>
#include <stddef.h>
int battery_voltage_parse(const char *, unsigned int *);
int main(void)
{
	const struct { const char *text; unsigned int value; } valid[] = {
		{"0", 0}, {"0.00", 0}, {"12.64", 1264}, {"1.2", 120},
		{"99.99", 9999}, {"09.01", 901}, {"12", 1200},
	};
	const char *invalid[] = {"", "-1", "100", "100.00", "nan", "inf",
		"12.345", "12.", ".5", "12.64junk", "12 64", " 12", "12 ",
		"999999999999999999999999", "+12", "1e1"};
	for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
		unsigned int value = 42;
		assert(battery_voltage_parse(valid[i].text, &value) == 0);
		assert(value == valid[i].value);
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		unsigned int value = 42;
		assert(battery_voltage_parse(invalid[i], &value) < 0);
		assert(value == 42);
	}
	return 0;
}
