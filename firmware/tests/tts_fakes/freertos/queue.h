#include "FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned, size_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
int xQueueOverwrite(QueueHandle_t, const void *);
void vQueueDelete(QueueHandle_t);
