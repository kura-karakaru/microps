#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "platform.h"
#include "util.h"

#define LOOPBACK_MTU UINT16_MAX /* maximum size of IP datagram */
#define LOOPBACK_QUEUE_LIMIT 16
#define LOOPBACK_IRQ (INTR_IRQ_BASE + 1)

#define PRIV(x) ((struct loopback *)x->priv)

// ループバックデバイスのドライバ内で使用するプライベートなデータを格納する構造体
struct loopback {
    int irq;
    mutex_t mutex;
    struct queue_head queue;
};

struct loopback_queue_entry {
    uint16_t type;
    size_t len;
    uint8_t data[]; /* flexible array member */
};

static int loopback_transmit(struct net_device *dev, uint16_t type,
                             const uint8_t *data, size_t len, const void *dst) {
    struct loopback_queue_entry *entry;
    unsigned int num;

    // queueへのアクセスをmutexで保護する
    mutex_lock(&PRIV(dev)->mutex);
    // queueの上限を超えていたらエラーを返す
    if (PRIV(dev)->queue.num >= LOOPBACK_QUEUE_LIMIT) {
        mutex_unlock(&PRIV(dev)->mutex);
        errorf("queue is full");
        return -1;
    }

    // キューに格納するエントリのメモリを確保（エントリ構造体＋データ長）
    // 便利ツールのキューはポインタを保持するだけ
    entry = memory_alloc(sizeof(*entry) + len);
    if (!entry) {
        mutex_unlock(&PRIV(dev)->mutex);
        errorf("memory_alloc() failure");
        return -1;
    }

    // メタデータの設定とデータ本体のコピー
    entry->type = type;
    entry->len = len;
    memcpy(entry->data, data, len);

    // エントリをキューへ格納
    queue_push(&PRIV(dev)->queue, entry);
    num = PRIV(dev)->queue.num;
    mutex_unlock(&PRIV(dev)->mutex);
    debugf("queue pushed (num:%u), dev=%s, type=0x%04x, len=%zd", num,
           dev->name, type, len);
    debugdump(data, len);
    intr_raise_irq(PRIV(dev)->irq);
    return 0;
}

static int loopback_isr(unsigned int irq, void *id) {
    struct net_device *dev;
    struct loopback_queue_entry *entry;

    dev = (struct net_device *)id;

    // キューへのアクセスをmutexで保護
    mutex_lock(&PRIV(dev)->mutex);
    while (1) {
        // キューからエントリを取り出す
        entry = queue_pop(&PRIV(dev)->queue);
        // 取り出すものがなくなったら終了
        if (!entry) {
            break;
        }
        debugf("queue popped (num:%u), dev=%s, type=0x%04x, len=%zd",
               PRIV(dev)->queue.num, dev->name, entry->type, entry->len);
        debugdump(entry->data, entry->len);

        // net_input_handler() に受信データ本体と付随する情報を渡す
        net_input_handler(entry->type, entry->data, entry->len, dev);
        // エントリのメモリを解放する
        memory_free(entry);
    }

    // mutexのunlockを忘れずに
    mutex_unlock(&PRIV(dev)->mutex);
    return 0;
}

static struct net_device_ops loopback_ops = {
    .transmit = loopback_transmit,
};

struct net_device *loopback_init(void) {
    struct net_device *dev;
    struct loopback *lo;

    // Ex.3-1: デバイス生成とパラメータ設定
    dev = net_device_alloc();
    if (!dev) {
        errorf("net_device_alloc() failure");
        return NULL;
    }

    /*
    ・type: net.h に定義されている定数を使う
    ・mtu: ソースファイル冒頭で定義している定数を使う
    ・hlen/alen: いずれも0を設定する
    ・flags: NET_DEVICE_FLAG_LOOPBACK を設定する
    */

    dev->type = NET_DEVICE_TYPE_LOOPBACK;
    dev->mtu = LOOPBACK_MTU;
    dev->hlen = 0;
    dev->alen = 0;
    dev->flags = NET_DEVICE_FLAG_LOOPBACK;

    // ドライバの中で使用するプライベートなデータの準備
    lo = memory_alloc(sizeof(*lo));
    if (!lo) {
        errorf("memory_alloc() failure");
        return NULL;
    }
    lo->irq = LOOPBACK_IRQ;
    mutex_init(&lo->mutex);
    queue_init(&lo->queue);

    // プライベートなデータをデバイス構造体に格納する（ドライバの関数が呼び出される際にはデバイス構造体が渡されるのでここから取り出す）
    dev->priv = lo;

    // Ex.3-2: デバイスの登録と割り込みハンドラの設定

    dev->ops =
        &loopback_ops;  // デバイスドライバが実装している関数のアドレスを保持する構造体へのポインタを設定する

    // デバイスを登録
    if (net_device_register(dev) == -1) {
        errorf("net_device_register() failure");
        return NULL;
    }

    // 割り込みハンドラとして loopback_isr を登録する
    intr_request_irq(LOOPBACK_IRQ, loopback_isr, INTR_IRQ_SHARED, dev->name,
                     dev);

    debugf("initialized, dev=%s", dev->name);
    return dev;
}