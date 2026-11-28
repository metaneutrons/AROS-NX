/* Copyright (c) 2026, The AROS Development Team.
 * Custom OSAL for the polled echo experiment ONLY. Every API asserts the
 * owning AROS task; no hardware USB ISR and no second task enters TinyUSB.
 * Calls are nonblocking. This is not a reusable SMP/IRQ OSAL implementation.
 */
#ifndef P4_ECHO_OSAL_H
#define P4_ECHO_OSAL_H
#include "p4_usb_platform.h"
typedef void *osal_task_handle_t;
static inline osal_task_handle_t osal_task_get_current_handle(void) { p4_usb_owner_check(); return (void *)1; }
static inline uint32_t osal_time_millis(void) { return p4_usb_millis(); }
static inline void osal_task_delay(uint32_t ms) { p4_usb_delay(ms); }
typedef struct { uint32_t nesting; } osal_spinlock_t;
#define OSAL_SPINLOCK_DEF(name, fn) osal_spinlock_t name = {0}
static inline void osal_spin_init(osal_spinlock_t *s) { p4_usb_owner_check(); s->nesting = 0; }
static inline void osal_spin_deinit(osal_spinlock_t *s) { p4_usb_owner_check(); if(s->nesting) p4_usb_fault("held lock"); }
static inline void osal_spin_lock(osal_spinlock_t *s, bool isr) { (void)isr; p4_usb_owner_check(); ++s->nesting; }
static inline void osal_spin_unlock(osal_spinlock_t *s, bool isr) { (void)isr; p4_usb_owner_check(); if(!s->nesting) p4_usb_fault("lock imbalance"); --s->nesting; }
typedef struct { uint16_t count; } osal_semaphore_def_t;
typedef osal_semaphore_def_t *osal_semaphore_t;
static inline osal_semaphore_t osal_semaphore_create(osal_semaphore_def_t *s) { p4_usb_owner_check(); s->count=0; return s; }
static inline bool osal_semaphore_delete(osal_semaphore_t s) { (void)s; p4_usb_owner_check(); return true; }
static inline bool osal_semaphore_post(osal_semaphore_t s, bool isr) { (void)isr; p4_usb_owner_check(); if(s->count==UINT16_MAX) p4_usb_fault("semaphore overflow"); ++s->count; return true; }
static inline bool osal_semaphore_wait(osal_semaphore_t s, uint32_t ms) { (void)ms; p4_usb_owner_check(); if(!s->count) return false; --s->count; return true; }
static inline void osal_semaphore_reset(osal_semaphore_t s) { p4_usb_owner_check(); s->count=0; }
typedef osal_semaphore_def_t osal_mutex_def_t;
typedef osal_semaphore_t osal_mutex_t;
static inline osal_mutex_t osal_mutex_create(osal_mutex_def_t *s) { p4_usb_owner_check(); s->count=1; return s; }
static inline bool osal_mutex_delete(osal_mutex_t s) { return osal_semaphore_delete(s); }
static inline bool osal_mutex_lock(osal_mutex_t s, uint32_t ms) { (void)ms; p4_usb_owner_check(); if(!s->count) p4_usb_fault("recursive mutex"); s->count=0; return true; }
static inline bool osal_mutex_unlock(osal_mutex_t s) { p4_usb_owner_check(); if(s->count) p4_usb_fault("mutex imbalance"); s->count=1; return true; }
#include "common/tusb_fifo.h"
typedef struct { uint16_t item_size; tu_fifo_t ff; } osal_queue_def_t;
typedef osal_queue_def_t *osal_queue_t;
#define OSAL_QUEUE_DEF(fn,name,depth,type) \
    uint8_t name##_buf[(depth)*sizeof(type)]; \
    osal_queue_def_t name = { .item_size=sizeof(type), .ff=TU_FIFO_INIT(name##_buf,(depth)*sizeof(type),false) }
static inline osal_queue_t osal_queue_create(osal_queue_def_t *q) { p4_usb_owner_check(); tu_fifo_clear(&q->ff); return q; }
static inline bool osal_queue_delete(osal_queue_t q) { (void)q; p4_usb_owner_check(); return true; }
static inline bool osal_queue_send(osal_queue_t q, const void *d, bool isr) { (void)isr; p4_usb_owner_check(); if(tu_fifo_remaining(&q->ff)<q->item_size) p4_usb_fault("USB event queue full"); return tu_fifo_write_n(&q->ff,d,q->item_size)==q->item_size; }
static inline bool osal_queue_receive(osal_queue_t q, void *d, uint32_t ms) { (void)ms; p4_usb_owner_check(); if(tu_fifo_count(&q->ff)<q->item_size) return false; return tu_fifo_read_n(&q->ff,d,q->item_size)==q->item_size; }
static inline bool osal_queue_empty(osal_queue_t q) { p4_usb_owner_check(); return tu_fifo_empty(&q->ff); }
#endif
