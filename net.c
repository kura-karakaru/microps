#include "net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/linux/platform.h"
#include "util.h"

/* NOTE: if you want to add/delete the entries after net_run(), you need to
 * protect these lists with a mutex. */
static struct net_device *devices;  // デバイスリスト（の先頭を指すポインタ）

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

int net_input_handler(uint16_t type, const uint8_t *data, size_t len,
                      struct net_device *dev) {
    // TODO: implement
    // 現時点では呼ばれたことがわかれば良い
    debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
    debugdump(data, len);
    return 0;
}

int net_run(void) {
    struct net_device *dev;

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
    debugf("shutting down");
}

int net_init(void) {
    // TODO: implement
    infof("initialized");
    return 0;
}
