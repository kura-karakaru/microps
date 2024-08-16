#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "driver/dummy.h"
#include "net.h"
#include "test.h"
#include "util.h"

static volatile sig_atomic_t terminate;

static void on_signal(int s) {
    (void)s;
    terminate = 1;
}

int main(int argc, char *argv[]) {
    struct net_device *dev;

    // シグナルハンドラの設定
    signal(SIGINT, on_signal);
    // プロトコルスタックの初期化
    if (net_init() == -1) {
        errorf("net_init() failure");
        return -1;
    }
    // ダミーデバイスの初期化
    dev = dummy_init();
    if (!dev) {
        errorf("dummy_init() failure");
        return -1;
    }
    // プロトコルスタックの起動
    if (net_run() == -1) {
        errorf("net_run() failure");
        return -1;
    }
    // Ctrl+Cが押されるとon_signal()でterminateに1が設定
    while (!terminate) {
        // 1秒おきに出力関数が動作
        // デバイスにパケットを書き込む
        // まだパケットを自力で生成できないのでテストデータを用いる
        if (net_device_output(dev, 0x0800, test_data, sizeof(test_data),
                              NULL) == -1) {
            errorf("net_device_output() failure");
            break;
        }
        sleep(1);
    }
    // プロトコルスタックの停止
    net_shutdown();
    return 0;
}
