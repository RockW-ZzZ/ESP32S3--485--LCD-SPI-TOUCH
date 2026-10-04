#include "rs485.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    uart_port_t uart;
    int tx_pin, rx_pin, de_pin;
    QueueHandle_t events;
    SemaphoreHandle_t lock;
    uint32_t gap_us;
    int baud_rate, bits_per_char;
    bool ready;
} rs485_context_t;

static rs485_context_t ports[RS485_PORT_COUNT] = {
    [RS485_PORT_1] = {.uart = UART_NUM_1, .tx_pin = BOARD_RS4851_TX,
                     .rx_pin = BOARD_RS4851_RX, .de_pin = BOARD_RS4851_DE_RE},
    [RS485_PORT_2] = {.uart = UART_NUM_2, .tx_pin = BOARD_RS4852_TX,
                     .rx_pin = BOARD_RS4852_RX, .de_pin = BOARD_RS4852_DE_RE},
};

static TickType_t ticks_from_us(uint64_t us)
{
    /* Extra tick avoids early expiry when called just before a tick boundary. */
    return (TickType_t)((us * configTICK_RATE_HZ + 999999) / 1000000 + 1);
}

esp_err_t rs485_init(rs485_port_t port, int baud, uart_parity_t parity, uart_stop_bits_t stop_bits)
{
    if ((unsigned)port >= RS485_PORT_COUNT) return ESP_ERR_INVALID_ARG;
    rs485_context_t *ctx = &ports[port];
    if (ctx->ready) return ESP_ERR_INVALID_STATE;
    if (baud < 1200 || baud > 115200 ||
        (parity != UART_PARITY_DISABLE && parity != UART_PARITY_EVEN && parity != UART_PARITY_ODD) ||
        (stop_bits != UART_STOP_BITS_1 && stop_bits != UART_STOP_BITS_2)) return ESP_ERR_INVALID_ARG;
    gpio_config_t de = {.pin_bit_mask = 1ULL << ctx->de_pin, .mode = GPIO_MODE_OUTPUT};
    esp_err_t err = gpio_config(&de);
    if (err == ESP_OK) err = gpio_set_level(ctx->de_pin, 0);
    if (err != ESP_OK) return err;
    ctx->lock = xSemaphoreCreateMutex();
    if (!ctx->lock) return ESP_ERR_NO_MEM;
    bool installed = false;
    uart_config_t cfg = {.baud_rate = baud, .data_bits = UART_DATA_8_BITS,
        .parity = parity, .stop_bits = stop_bits, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};
    err = uart_param_config(ctx->uart, &cfg);
    if (err != ESP_OK) goto fail;
    err = uart_driver_install(ctx->uart, 2048, 0, 32, &ctx->events, 0);
    if (err != ESP_OK) goto fail;
    installed = true;
    err = uart_set_pin(ctx->uart, ctx->tx_pin, ctx->rx_pin, ctx->de_pin, -1);
    if (err != ESP_OK) goto fail;
    err = uart_set_mode(ctx->uart, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) goto fail;
    /* uart_set_rts(1) is physical LOW, i.e. receive, in the IDF API. */
    err = uart_set_rts(ctx->uart, 1);
    if (err != ESP_OK) goto fail;
    ctx->bits_per_char = 1 + 8 + (parity != UART_PARITY_DISABLE) + (stop_bits == UART_STOP_BITS_2 ? 2 : 1);
    ctx->baud_rate = baud;
    ctx->gap_us = baud > 19200 ? 1750 : (3500000U * ctx->bits_per_char + baud - 1) / baud;
    unsigned symbols = (ctx->gap_us * (uint32_t)baud + ctx->bits_per_char * 1000000U - 1) /
                       (ctx->bits_per_char * 1000000U);
    err = uart_set_rx_timeout(ctx->uart, symbols);
    if (err != ESP_OK) goto fail;
    err = uart_set_rx_full_threshold(ctx->uart, 120);
    if (err != ESP_OK) goto fail;
    uart_set_always_rx_timeout(ctx->uart, true);
    ctx->ready = true;
    return ESP_OK;
fail:
    if (installed) uart_driver_delete(ctx->uart);
    ctx->events = NULL;
    vSemaphoreDelete(ctx->lock); ctx->lock = NULL;
    gpio_reset_pin(ctx->de_pin);
    gpio_set_direction(ctx->de_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(ctx->de_pin, 0);
    return err;
}

/* Discard stale bytes and wait for bus silence before starting a new request.
 * Bounded even if another transmitter is continuously sending. */
static esp_err_t wait_idle(rs485_context_t *ctx, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    /* Observe even a continuous stream shorter than the normal FIFO threshold;
     * otherwise the RX ring can appear empty while bytes still fill the FIFO. */
    /* IDF's always-timeout mode leaves one byte in FIFO. Disable it before
     * threshold=1, or that retained byte can continuously assert RX_FULL. */
    uart_set_always_rx_timeout(ctx->uart, false);
    esp_err_t err = uart_set_rx_full_threshold(ctx->uart, 1);
    if (err != ESP_OK) return err;
    uart_flush_input(ctx->uart);
    xQueueReset(ctx->events);
    int64_t idle_since = esp_timer_get_time();
    for (;;) {
        uart_event_t event;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return ESP_ERR_TIMEOUT;
        TickType_t wait = ticks_from_us(ctx->gap_us * 2U);
        TickType_t limit = ticks_from_us(remaining);
        if (wait > limit) wait = limit;
        if (xQueueReceive(ctx->events, &event, wait) != pdTRUE) {
            size_t buffered = 0;
            err = uart_get_buffered_data_len(ctx->uart, &buffered);
            if (err != ESP_OK) return err;
            if (!buffered && esp_timer_get_time() - idle_since >= ctx->gap_us * 2U)
                return ESP_OK;
            if (!buffered) continue;
        }
        uart_flush_input(ctx->uart);
        xQueueReset(ctx->events);
        idle_since = esp_timer_get_time();
    }
}

esp_err_t rs485_exchange(rs485_port_t port, const uint8_t *tx, size_t tx_length,
                         uint8_t *rx, size_t capacity, size_t *rx_length,
                         uint32_t timeout_ms)
{
    if (rx_length) *rx_length = 0;
    if ((unsigned)port >= RS485_PORT_COUNT || !tx || tx_length < 4 || tx_length > 256 || !rx || !rx_length ||
        capacity < 4 || capacity > 256 || !timeout_ms || timeout_ms > 60000)
        return ESP_ERR_INVALID_ARG;
    rs485_context_t *ctx = &ports[port];
    if (!ctx->ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(ctx->lock, ticks_from_us(timeout_ms * 1000ULL)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = wait_idle(ctx, timeout_ms);
    esp_err_t threshold_err = uart_set_rx_full_threshold(ctx->uart, 120);
    uart_set_always_rx_timeout(ctx->uart, true);
    if (err == ESP_OK) err = threshold_err;
    if (err != ESP_OK) goto done;
    if (uart_write_bytes(ctx->uart, tx, tx_length) != (int)tx_length) {
        err = ESP_FAIL; goto done;
    }
    err = uart_wait_tx_done(ctx->uart, ticks_from_us(
        tx_length * ctx->bits_per_char * 1000000ULL / ctx->baud_rate + 100000));
    if (err != ESP_OK) goto done;
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    size_t length = 0;
    for (;;) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) { err = ESP_ERR_TIMEOUT; break; }
        uart_event_t event;
        if (xQueueReceive(ctx->events, &event, ticks_from_us(remaining)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT; break;
        }
        if (event.type == UART_DATA) {
            if (event.size > capacity - length) { err = ESP_ERR_INVALID_SIZE; break; }
            if (event.size) {
                int n = uart_read_bytes(ctx->uart, rx + length, event.size, 0);
                if (n != (int)event.size) { err = ESP_FAIL; break; }
                length += n;
            }
            /* A FIFO-full event is only a fragment. Complete on hardware idle. */
            if (event.timeout_flag && length) {
                *rx_length = length;
                err = ESP_OK; break;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            err = ESP_ERR_INVALID_SIZE; break;
        } else if (event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR || event.type == UART_BREAK) {
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
    }
done:
    if (err != ESP_OK) { uart_flush_input(ctx->uart); xQueueReset(ctx->events); }
    xSemaphoreGive(ctx->lock);
    return err;
}
