#ifndef WEBSERVER_H
#define WEBSERVER_H
#include "http_parser.h"
#include "freertos/task.h"
//#include "stream.h"
#include "freertos/event_groups.h"
#include "driver/sdmmc_types.h"

void webserver_task( void *pvParameters );

struct webserver_params
{
 char * html;
 char * text;
 char * voice;
 EventGroupHandle_t eventGroup;
 esp_err_t err;
 char * errorText;
};

#endif // WEBSERVER_H
