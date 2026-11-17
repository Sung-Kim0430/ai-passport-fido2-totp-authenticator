#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "auth_identity.h"

int main(void)
{
    const uint8_t chip_a[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t chip_b[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x56};
    auth_identity_t a, again, b;
    assert(auth_identity_from_chip_id(chip_a, &a));
    assert(auth_identity_from_chip_id(chip_a, &again));
    assert(auth_identity_from_chip_id(chip_b, &b));
    assert(strcmp(a.device_id, "021122334455") == 0);
    assert(strcmp(a.ssid, again.ssid) == 0);
    assert(strcmp(a.password, again.password) == 0);
    assert(strcmp(a.password, b.password) != 0);
    assert(strlen(a.password) == 8);
    assert(sizeof(a.password) == 9);
    assert(strlen(a.ssid) == 15);
    assert(strcmp(a.ssid, "Passport-334455") == 0);
    assert(strchr(a.ssid + 9, '-') == NULL);
    assert(!auth_identity_from_chip_id(NULL, &a));
    assert(!auth_identity_from_chip_id(chip_a, NULL));
    puts("Chip-derived hotspot identity: PASS");
    return 0;
}
