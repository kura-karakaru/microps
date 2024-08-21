#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "platform.h"
#include "util.h"

struct irq_entry {
    struct irq_entry *next;  // 次のIRQ構造体へのポインタ
    unsigned int irq;        // IRQ番号
    int (*handler)(
        unsigned int irq,
        void *
            dev);  // 割り込みハンドラ（割り込みが発生した際に呼び出す関数へのポインタ）
    int flags;  // フラグ　INTR_IRQ_SHAREDが指定された場合はIRQ番号を共有可能
    char name[16];  // デバッグ出力で識別するための名前
    void *dev;      // 割り込みの発生元デバイス
};

/* NOTE: if you want to add/delete the entries after intr_run(), you need to
 * protect these lists with a mutex. */
static struct irq_entry *irqs;  // IRQリストの先頭を指すポインタ

static sigset_t sigmask;  // シグナル集合

static pthread_t tid;  // 割り込み処理スレッドのスレッドID
static pthread_barrier_t barrier;

int intr_request_irq(unsigned int irq,
                     int (*handler)(unsigned int irq, void *dev), int flags,
                     const char *name, void *dev) {
    struct irq_entry *entry;
    debugf("irq=%u, flags=%d, name=%s", irq, flags, name);

    /*
    IRQ番号が既に登録されている場合、IRQ番号の共有が許可されているかどうかチェック。
    どちらかが共有を許可してない場合はエラーを返す。
    */
    for (entry = irqs; entry; entry = entry->next) {
        if (entry->irq == irq) {
            if (entry->flags ^ INTR_IRQ_SHARED || flags ^ INTR_IRQ_SHARED) {
                errorf("conflicts with already registered IRQs");
                return -1;
            }
        }
    }
    // IRQリストに新しいエントリを追加

    // 新しいエントリのメモリ確保
    entry = memory_alloc(sizeof(*entry));
    if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
    }

    // IRQ構造体に名前を指定
    entry->irq = irq;
    entry->handler = handler;
    entry->flags = flags;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->dev = dev;

    // IRQリストの先頭へ挿入
    entry->next = irqs;
    irqs = entry;

    // シグナル集合へ新しいシグナルを追加
    sigaddset(&sigmask, irq);
    debugf("registered: irq=%u, name=%s", irq, name);

    return 0;
}

int intr_raise_irq(unsigned int irq) {
    return pthread_kill(tid, (int)irq);  // 割り込み処理スレッドへシグナルを送信
}

static void *intr_thread(void *arg) {
    int terminate = 0, sig, err;
    struct irq_entry *entry;

    debugf("start...");
    pthread_barrier_wait(&barrier);  // メインスレッドと同期をとる
    while (!terminate) {
        // 割り込みに見立てたシグナルの発生まで待機
        err = sigwait(&sigmask, &sig);
        if (err) {
            errorf("sigwait() %s", strerror(err));
            break;
        }
        switch (sig) {
            // SIGHUP: 割り込みスレッドへ終了を通知するためのシグナル
            // terminate を 1 にしてループを抜ける
            case SIGHUP:
                terminate = 1;
                break;

            // ソフトウェア割り込み用のシグナル（SIGUSR1）を捕捉したらnet_softirq_handler()を呼ぶ
            case SIGUSR1:
                net_softirq_handler();
                break;

            // デバイス割り込み用のシグナル
            default:
                for (entry = irqs; entry;
                     entry = entry->next) {  // IRQリストを巡回
                    // IRQ番号が一致するエントリの割り込みハンドラを呼び出す
                    if (entry->irq == (unsigned int)sig) {
                        debugf("irq=%d, name=%s", entry->irq, entry->name);
                        entry->handler(entry->irq, entry->dev);
                    }
                }
                break;
        }
    }
    debugf("terminated");
    return NULL;
}

int intr_run(void) {
    int err;

    // シグナルマスクの設定
    err = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
    if (err) {
        errorf("pthread_sigmask() %s", strerror(err));
        return -1;
    }

    // 割り込み処理スレッドの起動
    err = pthread_create(&tid, NULL, intr_thread, NULL);
    if (err) {
        errorf("pthread_create() %s", strerror(err));
        return -1;
    }

    /*
    スレッドが動き出すまで待つ
    他のスレッドが同じようにpthread_barrier_wait()を呼び出し、バリアのカウントが指定した数になるまでスレッドを停止
    */
    pthread_barrier_wait(&barrier);
    return 0;
}

void intr_shutdown(void) {
    // 割り込み処理スレッドが起動済みかどうかを確認
    if (pthread_equal(tid, pthread_self()) != 0) {
        /* Thread not created. */
        return;
    }

    // 割り込み処理スレッドにシグナル（SIGHUP）を送信
    pthread_kill(tid, SIGHUP);
    // 割り込み処理スレッドが完全に終了するのを待つ
    pthread_join(tid, NULL);
}

int intr_init(void) {
    tid = pthread_self();  // スレッドIDの初期値にメインスレッドのIDを指定
    pthread_barrier_init(&barrier, NULL,
                         2);  // pthread_barrierの初期化 カウントを2に
    sigemptyset(&sigmask);    // シグナル集合を初期化
    sigaddset(&sigmask, SIGHUP);  // シグナル集合にSIGHUPを追加
    sigaddset(&sigmask, SIGUSR1);  // ソフトウェア割り込みのSIGUSR1を追加
    return 0;
}
