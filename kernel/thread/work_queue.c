#include "work_queue.h"

#include <string.h>

#include "scheduler.h"
#include "mem/phys.h"

static void work_queue_worker(void* _ctx) {
    work_queue_t* ctx = (work_queue_t*)_ctx;

    for (;;) {
        // wait for work atomically, if there is work already
        // then don't wait and take it immediately
        bool irq_status = irq_spinlock_lock(&ctx->lock);
        if (ctx->head == NULL) {
            cond_wait_irq(&ctx->cond, &ctx->lock, &irq_status);
        }
        work_queue_item_t* head = ctx->head;
        ctx->head = NULL;
        ctx->tail = NULL;
        irq_spinlock_unlock(&ctx->lock, irq_status);

        // run all of the work items, free them as we go
        while (head != NULL) {
            work_queue_item_t* next = head->next;
            head->handler(head->ctx);
            phys_free(head);
            head = next;
        }
    }
}

err_t init_work_queue(work_queue_t* queue, const char* fmt, ...) {
    err_t err = NO_ERROR;

    memset(queue, 0, sizeof(*queue));

    va_list va;
    va_start(va, fmt);
    queue->worker = thread_vcreate(work_queue_worker, queue, fmt, va);
    va_end(va);
    scheduler_start_thread(queue->worker);

cleanup:
    return err;
}

err_t work_queue_add(work_queue_t* queue, void (*handler)(void* ctx), void* ctx) {
    err_t err = NO_ERROR;
    bool locked = false;

    CHECK(queue->worker != NULL);

    // create the item, use the phys_alloc since it is interrupt safe
    work_queue_item_t* item = phys_alloc(sizeof(*item));
    CHECK_ERROR(item != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    item->next = NULL;
    item->handler = handler;
    item->ctx = ctx;

    bool irq_status = irq_spinlock_lock(&queue->lock);
    locked = true;

    // queue it to the end
    if (queue->head == NULL) {
        queue->head = item;
    } else {
        queue->tail->next = item;
    }
    queue->tail = item;

    // wakeup the worker
    cond_signal(&queue->cond);

cleanup:
    if (locked) {
        irq_spinlock_unlock(&queue->lock, irq_status);
    }

    return err;
}

