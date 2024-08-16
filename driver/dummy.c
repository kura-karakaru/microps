#include "dummy.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "net.h"
#include "platform.h"
#include "util.h"

#define DUMMY_MTU \
    UINT16_MAX  // ダミーデバイスの MTU（IPデータグラムの最大サイズ）

#define DUMMY_IRQ INTR_IRQ_BASE

static int dummy_transmit(struct net_device *dev, uint16_t type,
                          const uint8_t *data, size_t len, const void *dst) {
    debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
    debugdump(data, len);
    /* drop data */
    // データの破棄
    return 0;
}

static int dummy_isr(unsigned int irq, void *id) {}

static struct net_device_ops dummy_ops = {
    .transmit = dummy_transmit,  // 送信関数のみ設定
};

struct net_device *dummy_init(void) {
    struct net_device *dev;

    dev = net_device_alloc();
    if (!dev) {
        errorf("net_device_alloc() failure");
        return NULL;
    }

    // net.h で定義
    dev->type = NET_DEVICE_TYPE_DUMMY;
    dev->mtu = DUMMY_MTU;

    // ヘッダもアドレスも存在しないこととして明示的に0に設定
    dev->hlen = 0; /* non header */
    dev->alen = 0; /* non address */

    dev->ops =
        &dummy_ops;  // デバイスドライバが実装している関数のアドレスを保持する構造体へのポインタを設定する

    // デバイスを登録
    if (net_device_register(dev) == -1) {
        errorf("net_device_register() failure");
        return NULL;
    }
    debugf("initialized, dev=%s", dev->name);
    return dev;
}
