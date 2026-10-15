/* Minimal host mock of the IDF system-reset entry point. */
#ifndef C6_DISCOVERY_FIXTURE_ESP_SYSTEM_H
#define C6_DISCOVERY_FIXTURE_ESP_SYSTEM_H

void esp_restart(void) __attribute__((noreturn));

#endif
