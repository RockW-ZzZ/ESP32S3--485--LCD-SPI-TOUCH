#include "mock_idf.h"
#include "button_filter.h"
#include "touch_gate.h"
/* Python supplies real host locks, queues and monotonic time. Production driver
 * transactions execute on separate native threads via ctypes.CDLL. */
typedef int64_t (*hook_t)(int, intptr_t, intptr_t, intptr_t, intptr_t);
static hook_t hook;
__declspec(dllexport) void mock_set_hook(hook_t h) { hook = h; }
#define H(op,a,b,c,d) hook(op,(intptr_t)(a),(intptr_t)(b),(intptr_t)(c),(intptr_t)(d))
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)(intptr_t)H(1,0,0,0,0); }
int xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { return (int)H(2,h,t,0,0); }
int xSemaphoreGive(SemaphoreHandle_t h) { return (int)H(3,h,0,0,0); }
void vSemaphoreDelete(SemaphoreHandle_t h) { H(4,h,0,0,0); }
int xQueueReceive(QueueHandle_t h, void *v, TickType_t t) { return (int)H(5,h,v,t,0); }
int xQueueReset(QueueHandle_t h) { return (int)H(6,h,0,0,0); }
int64_t esp_timer_get_time(void) { return H(7,0,0,0,0); }
esp_err_t uart_param_config(uart_port_t p, const uart_config_t *c) { return (int)H(8,p,c,0,0); }
esp_err_t uart_driver_install(uart_port_t p, int r, int t, int n, QueueHandle_t *q, int f)
{ (void)r; (void)t; (void)f; return (int)H(9,p,n,q,0); }
esp_err_t uart_driver_delete(uart_port_t p) { return (int)H(10,p,0,0,0); }
esp_err_t uart_set_pin(uart_port_t p, int t, int r, int de, int cts)
{ (void)cts; return (int)H(11,p,t,r,de); }
esp_err_t uart_set_mode(uart_port_t p, int v) { return (int)H(12,p,v,0,0); }
esp_err_t uart_set_rts(uart_port_t p, int v) { return (int)H(13,p,v,0,0); }
esp_err_t uart_set_rx_timeout(uart_port_t p, uint8_t v) { return (int)H(14,p,v,0,0); }
esp_err_t uart_set_rx_full_threshold(uart_port_t p, int v) { return (int)H(15,p,v,0,0); }
void uart_set_always_rx_timeout(uart_port_t p, bool v) { H(16,p,v,0,0); }
esp_err_t uart_flush_input(uart_port_t p) { return (int)H(17,p,0,0,0); }
esp_err_t uart_get_buffered_data_len(uart_port_t p, size_t *n) { return (int)H(18,p,n,0,0); }
int uart_write_bytes(uart_port_t p, const void *b, size_t n) { return (int)H(19,p,b,n,0); }
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t t) { return (int)H(20,p,t,0,0); }
int uart_read_bytes(uart_port_t p, void *b, uint32_t n, TickType_t t) { return (int)H(21,p,b,n,t); }
esp_err_t gpio_config(const gpio_config_t *c) { return (int)H(22,c,0,0,0); }
esp_err_t gpio_set_level(int p, uint32_t v) { return (int)H(23,p,v,0,0); }
esp_err_t gpio_reset_pin(int p) { return (int)H(24,p,0,0,0); }
esp_err_t gpio_set_direction(int p, int v) { return (int)H(25,p,v,0,0); }

__declspec(dllexport) unsigned test_button_step(button_filter_t *s, bool sample, int64_t now, int key)
{
    return button_filter_step(s, sample, now,
        key == 0 ? BUTTON_KEY1_FILTER_MS : BUTTON_KEY2_FILTER_MS,
        key == 0 ? 0 : BUTTON_KEY2_HOLD_MS);
}
__declspec(dllexport) void test_gate_set(touch_gate_t *s, bool en) { touch_gate_set(s, en); }
__declspec(dllexport) bool test_gate_filter(touch_gate_t *s, bool updated, uint8_t count, bool *reset)
{ return touch_gate_filter(s, updated, count, reset); }
