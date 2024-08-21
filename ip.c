#include "ip.h"

#include <stddef.h>
#include <stdint.h>

#include "net.h"
#include "util.h"

static void ip_input(const uint8_t *data, size_t len, struct net_device *dev) {
    debugf("dev=%s, len=%zu", dev->name, len);
    debugdump(data, len);
}

int ip_init(void) {}