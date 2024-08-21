#include "net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ip.h"
#include "platform/linux/platform.h"
#include "util.h"

#define PRIV(x) ((struct net_protocol *)x->priv)

struct net_protocol {
    struct net_protocol *next;  // 次プロトコルへのポインタ
    uint16_t type;              // プロトコルの種別
    struct queue_head queue; /* input queue */  // 受信キュー
    void (*handler)(
        const uint8_t *data, size_t len,
        struct net_device *dev);  // プロトコルの入力関数へのポインタ
};

struct net_protocol_queue_entry {  // 受信キューのエントリの構造体
    struct net_device *dev;
    size_t len;
    uint8_t data[];
};

/* NOTE: if you want to add/delete the entries after net_run(), you need to
 * protect these lists with a mutex. */
static struct net_device *devices;  // デバイスリスト（の先頭を指すポインタ）
static struct net_protocol *protocols;

struct net_device *net_device_alloc(void) {
    struct net_device *dev;

    // デバイス構造体のサイズのメモリを確保 確保した領域は0で初期化
    // メモリが確保できなかったらエラーとしてNULLを返す
    dev = memory_alloc(sizeof(*dev));
    if (!dev) {
        errorf("memory_alloc() failure");
        return NULL;
    }
    return dev;
}

/* NOTE: must not be call after net_run() */
int net_device_register(struct net_device *dev) {
    static unsigned int index = 0;

    dev->index = index++;  // デバイスのインデックス番号を設定
    snprintf(dev->name, sizeof(dev->name), "net%d",
             dev->index);  // デバイス名を生成

    // デバイスリストの先頭に追加
    dev->next = devices;
    devices = dev;

    infof("registered, dev=%s, type=0x%04x", dev->name, dev->type);
    return 0;
}

static int net_device_open(struct net_device *dev) {
    // デバイスの状態を確認 既にUPならエラー
    if (NET_DEVICE_IS_UP(dev)) {
        errorf("already opened, dev=%s", dev->name);
        return -1;
    }

    // デバイスドライバのオープン関数を呼び出す
    // オープン関数が設定されていなかったらスキップ
    // エラーを返されたらこの関数もエラーを返す
    if (dev->ops->open) {
        if (dev->ops->open(dev) == -1) {
            errorf("failure, dev=%s", dev->name);
            return -1;
        }
    }

    dev->flags |= NET_DEVICE_FLAG_UP;  // UPフラグを立てる
    infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

static int net_device_close(struct net_device *dev) {
    // デバイスの状態を確認 UP出ない場合はエラー
    if (!NET_DEVICE_IS_UP(dev)) {
        errorf("not opened, dev=%s", dev->name);
        return -1;
    }

    // デバイスドライバのクローズ関数を呼び出す
    // クローズ関数が設定されていなかったらスキップ
    // エラーを返されたらこの関数もエラーを返す
    if (dev->ops->close) {
        if (dev->ops->close(dev) == -1) {
            errorf("failure, dev=%s", dev->name);
            return -1;
        }
    }

    dev->flags &= ~NET_DEVICE_FLAG_UP;  // UPフラグを落とす
    infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

int net_device_output(struct net_device *dev, uint16_t type,
                      const uint8_t *data, size_t len, const void *dst) {
    // デバイスの状態を確認 UPでなければ送信できないのでエラー
    if (!NET_DEVICE_IS_UP(dev)) {
        errorf("not opened, dev=%s", dev->name);
        return -1;
    }

    // データサイズを確認
    // デバイスのMTUを超えるサイズのデータは送信できないのでエラー
    if (len > dev->mtu) {
        errorf("too long, dev=%s, mtu=%u, len=%zu", dev->name, dev->mtu, len);
        return -1;
    }
    debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
    debugdump(data, len);

    // デバイスドライバの出力関数を呼び出す エラーを返されたらこの関数もエラー
    if (dev->ops->transmit(dev, type, data, len, dst) == -1) {
        errorf("device transmit failure, dev=%s, len=%zu", dev->name, len);
        return -1;
    }
    return 0;
}

/* NOTE: must not be call after net_run() */
int net_protocol_register(uint16_t type,
                          void (*handler)(const uint8_t *data, size_t len,
                                          struct net_device *dev)) {
    struct net_protocol *proto;

    // 重複登録を確認する
    for (proto = protocols; proto; proto = proto->next) {
        if (type == proto->type) {
            errorf("already registered, type=0x%04x", type);
            return -1;
        }
    }

    // プロトコル構造体のメモリを確保
    proto = memory_alloc(sizeof(*proto));
    if (!proto) {
        errorf("memory_alloc() failure");
        return -1;
    }

    proto->type = type;
    proto->handler = handler;
    proto->next = protocols;
    protocols = proto;
    infof("registered, type=0x%04x", type);
    return 0;
}

int net_input_handler(uint16_t type, const uint8_t *data, size_t len,
                      struct net_device *dev) {
    struct net_protocol *proto;
    struct net_protocol_queue_entry *entry;

    for (proto = protocols; proto; proto = proto->next) {
        if (proto->type == type) {
            /*
                Exercise 4-1: プロトコルの受信キューにエントリを挿入
                (1) 新しいエントリのメモリを確保（失敗したらエラーを返す）
                (2) 新しいエントリへメタデータの設定と受信データのコピー
                (3) キューに新しいエントリを挿入（失敗したらエラーを返す）
            */

            entry = memory_alloc(sizeof(*entry) + len);
            if (!entry) {
                errorf("memory_alloc() failure");
                return -1;
            }

            entry->dev = dev;
            entry->len = len;
            memcpy(entry->data, data, len);

            // エントリをキューへ格納
            if (!queue_push(&proto->queue, entry)) {
                errorf("queue_push() failure");
                return -1;
            }

            debugf("queue pushed (num:%u), dev=%s, type=0x%04x, len=%zu",
                   proto->queue.num, dev->name, type, len);
            debugdump(data, len);

            // プロトコルの受信キューへエントリを追加した後、ソフトウェア割り込みを発生させる
            intr_raise_irq(INTR_IRQ_SOFTIRQ);

            return 0;
        }
    }
    /* unsupported protocol */
    return 0;
}

// ソフトウェア割り込み発生時に呼び出される関数
int net_softirq_handler(void) {
    struct net_protocol *proto;
    struct net_protocol_queue_entry *entry;

    // プロトコルリストを巡回（全てのプロトコルを確認）
    for (proto = protocols; proto; proto = proto->next) {
        while (1) {
            entry = queue_pop(&proto->queue);  // 1つずつ取り出す
            if (!entry) {
                break;  // 最後まで行ったら(queueから未処理のエントリがなくなったら)ループを抜ける
            }
            debugf("queue popped (num:%u), dev=%s, type=0x%04x, len=%zu",
                   proto->queue.num, entry->dev->name, proto->type, entry->len);
            debugdump(entry->data, entry->len);

            // プロトコルの入力関数
            proto->handler(entry->data, entry->len, entry->dev);
            // 使い終わったエントリのメモリを解放
            memory_free(entry);
        }
    }
    return 0;
}

int net_run(void) {
    struct net_device *dev;

    if (intr_run() == -1) {
        errorf("intr_run() failure");
        return -1;
    }

    debugf("open all devices...");

    // 登録済み全デバイスをオープン
    for (dev = devices; dev; dev = dev->next) {
        net_device_open(dev);
    }
    debugf("running...");
    return 0;
}

void net_shutdown(void) {
    struct net_device *dev;

    debugf("close all devices...");

    // 登録済み全デバイスをクローズ
    for (dev = devices; dev; dev = dev->next) {
        net_device_close(dev);
    }

    intr_shutdown();
    debugf("shutting down");
}

int net_init(void) {
    if (intr_init() == -1) {
        errorf("intr_init() failure");
        return -1;
    }
    if (ip_init() == -1) {
        errorf("ip_init() failure");
        return -1;
    }

    infof("initialized");
    return 0;
}
