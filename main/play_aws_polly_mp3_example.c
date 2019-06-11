/* Play MP3 Stream from Amazon Polly Text to Speech service

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <sys/time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "lwip/err.h"
#include "lwip/apps/sntp.h"
#include "esp_http_client.h"
#include "aws_sig_v4_signing.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_touch.h"
#include "webserver.h"
#include <tcpip_adapter.h>

#define AWS_POLLY_ENDPOINT "https://polly."CONFIG_AWS_POLLY_REGION".amazonaws.com/v1/speech"
#define TTS_TEXT  "Espressif Systems is a multinational, fabless semiconductor company, with headquarters in Shanghai, China. We specialize in producing highly-integrated, low-power, WiFi-and-Bluetooth IoT solutions. Among our most popular chips are ESP8266 and ESP32. We are committed to creating IoT solutions that are power-efficient, robust and secure."
static char *polly_payload = "{\"OutputFormat\":\"mp3\",\"SampleRate\":\"22050\",\"Text\":\""TTS_TEXT"\",\"TextType\":\"text\",\"VoiceId\":\"Joanna\"}";

static const char *TAG = "AWS_POLLY_EXAMPLE";
static EventGroupHandle_t eventGroup;
static audio_pipeline_handle_t pipeline;
static audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;
void play_text(bool bInit);
static audio_event_iface_handle_t evt;
static esp_periph_set_handle_t set;
static esp_periph_handle_t touch_periph;
int player_volume;
static audio_board_handle_t board_handle;

static void wait_for_sntp(void)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;

    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    const int retry_count = 20;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}


aws_sig_v4_context_t sigv4_context;
aws_sig_v4_config_t polly_sigv4_config = {
    .service_name = "polly",
    .region_name = CONFIG_AWS_POLLY_REGION,
    .access_key = CONFIG_AWS_ACCESS_KEY,
    .secret_key = CONFIG_AWS_SECRET_KEY,
    .host = "polly."CONFIG_AWS_POLLY_REGION".amazonaws.com",
    .method = "POST",
    .path = "/v1/speech",
    .query = "",
    .signed_headers = "content-type",
    .canonical_headers = "content-type:application/json\n",
};

int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    static int total_len = 0;
    esp_http_client_handle_t http_client = (esp_http_client_handle_t)msg->http_client;
    if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
        total_len += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes read: %d bytes\n", total_len);
        return ESP_OK;
    }
    if (msg->event_id != HTTP_STREAM_PRE_REQUEST) {
        return ESP_OK;
    }
    total_len = 0;
    struct timeval tv;
    time_t nowtime;
    struct tm *nowtm = NULL;
    gettimeofday(&tv, NULL);
    nowtime = tv.tv_sec;
    nowtm = localtime(&nowtime);
    char amz_date[32];
    char date_stamp[32];

    strftime(amz_date, sizeof amz_date, "%Y%m%dT%H%M%SZ", nowtm);
    strftime(date_stamp, sizeof date_stamp, "%Y%m%d", nowtm);

    int payload_len = strlen(polly_payload);

    polly_sigv4_config.payload = polly_payload;
    polly_sigv4_config.payload_len = payload_len;
    polly_sigv4_config.amz_date = amz_date;
    polly_sigv4_config.date_stamp = date_stamp;

    char *auth_header = aws_sig_v4_signing_header(&sigv4_context, &polly_sigv4_config);

    ESP_LOGI(TAG, "%s, amz_date=%s, date=%s", auth_header, amz_date, date_stamp);

    esp_http_client_set_post_field(http_client, polly_payload, payload_len);
    esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    esp_http_client_set_header(http_client, "X-Amz-Date", amz_date);
    return ESP_OK;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    tcpip_adapter_init();

    ESP_LOGI(TAG, "[ 1 ] Mount sdcard");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    /*esp_periph_set_handle_t*/ set = esp_periph_set_init(&periph_cfg);
    //esp_periph_init(&periph_cfg);

    // Initialize SD Card peripheral
    periph_sdcard_cfg_t sdcard_cfg = {
        .root = "/sdcard",
        .card_detect_pin = GPIO_NUM_34,
    };
    esp_periph_handle_t sdcard_handle = periph_sdcard_init(&sdcard_cfg);
    // Start sdcard & button peripheral
    esp_periph_start(set, sdcard_handle);

    // Wait until sdcard was mounted
    while (!periph_sdcard_is_mounted(sdcard_handle)) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
#ifdef CONFIG_FATFS_LFN_NONE
        ESP_LOGE(TAG, "long filenames must be configured!");
#endif

    ESP_LOGI(TAG, "Initialize Touch peripheral");
    periph_touch_cfg_t touch_cfg = {
        .touch_mask = BIT(get_input_set_id()) | BIT(get_input_play_id()) | BIT(get_input_volup_id()) | BIT(get_input_voldown_id()),
        .tap_threshold_percent = 70,
    };
    touch_periph = periph_touch_init(&touch_cfg);

    ESP_LOGI(TAG, " Start all peripherals");
    esp_periph_start(set, touch_periph);

    ESP_LOGI(TAG, "[ 0 ] Start and wait for Wi-Fi network");
    //esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    //periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    //esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    wait_for_sntp();

    tcpip_adapter_ip_info_t ipInfo;
    char str[256];
    // IP address.
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
    sprintf(str, "%x", ipInfo.ip.addr);

    struct webserver_params wsp;
    wsp.html = "/sdcard/www/play_aws_polly.html";
    //wsp.text = TTS_TEXT;
    wsp.text =  malloc(sizeof(TTS_TEXT)+1);
    strcpy(wsp.text, TTS_TEXT);
    wsp.voice = "Kimberly";
    eventGroup = xEventGroupCreate();
    wsp.eventGroup = eventGroup;
    wsp.err = ESP_OK;
    wsp.errorText="No error";

    /*start webserver*/
    xTaskCreate(webserver_task, "web_server_task", 4096, &wsp, +6, NULL);
    xEventGroupClearBits(wsp.eventGroup, 0xff);

   bool bInit = true;
   while (true) {

        // wait for web server to provide text
        ESP_LOGI(TAG, "wait for web page");
        xEventGroupWaitBits(eventGroup,BIT0,pdTRUE,pdTRUE,portMAX_DELAY);
        //polly_payload = "{\"OutputFormat\":\"mp3\",\"SampleRate\":\"22050\",\"Text\":\""TTS_TEXT"\",\"TextType\":\"text\",\"VoiceId\":\"Joanna\"}";
        //free(polly_payload);
        asprintf(&polly_payload, "{\"OutputFormat\":\"mp3\",\"SampleRate\":\"22050\",\"Text\":\"%s\",\"TextType\":\"text\",\"VoiceId\":\"%s\"}", wsp.text, wsp.voice);
        ESP_LOGI(TAG, "polly_payload: %s", polly_payload);

        play_text(bInit);
        bInit = false;

    }
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, mp3_decoder);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    esp_periph_set_destroy(set);
}

void play_text(bool bInit)
{

    //audio_pipeline_handle_t pipeline;
    //audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;
    if(bInit)
    {
        ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
        board_handle = audio_board_init();
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

        ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        pipeline = audio_pipeline_init(&pipeline_cfg);
        mem_assert(pipeline);

        ESP_LOGI(TAG, "[2.1] Create http stream to read data");
        http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
        http_cfg.event_handle = _http_stream_event_handle;
        http_cfg.type = AUDIO_STREAM_READER;
        http_stream_reader = http_stream_init(&http_cfg);

        ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
        i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
        i2s_cfg.type = AUDIO_STREAM_WRITER;
        i2s_stream_writer = i2s_stream_init(&i2s_cfg);


        ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        mp3_decoder = mp3_decoder_init(&mp3_cfg);

        ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
        audio_pipeline_register(pipeline, http_stream_reader, "http");
        audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
        audio_pipeline_register(pipeline, i2s_stream_writer,  "i2s");

        ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
        audio_pipeline_link(pipeline, (const char *[]) {"http", "mp3", "i2s"}, 3);

        ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
        audio_element_set_uri(http_stream_reader, AWS_POLLY_ENDPOINT);

        ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
        audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
        /*audio_event_iface_handle_t*/ evt = audio_event_iface_init(&evt_cfg);

        ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
        audio_pipeline_set_listener(pipeline, evt);

        ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
        audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);
    }

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    i2s_stream_set_clk(i2s_stream_writer, 22050, 16, 1);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
        if (msg.source_type == PERIPH_ID_TOUCH
                   && msg.cmd == PERIPH_TOUCH_TAP
                   && msg.source == (void *)touch_periph) {

                   if ((int) msg.data == get_input_play_id()) {
                       ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
#if 0
                       audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                       switch (el_state) {
                           case AEL_STATE_INIT :
                               ESP_LOGI(TAG, "[ * ] Starting audio pipeline");
                               audio_pipeline_run(pipeline);
                               break;
                           case AEL_STATE_RUNNING :
                               ESP_LOGI(TAG, "[ * ] Pausing audio pipeline");
                               audio_pipeline_pause(pipeline);
                               break;
                           case AEL_STATE_PAUSED :
                               ESP_LOGI(TAG, "[ * ] Resuming audio pipeline");
                               audio_pipeline_resume(pipeline);
                               break;
                           case AEL_STATE_FINISHED :
                               ESP_LOGI(TAG, "[ * ] Rewinding audio pipeline");
                               audio_pipeline_stop(pipeline);
                               adf_music_mp3_pos = 0;
                               audio_pipeline_resume(pipeline);
                               break;
                           default :
                               ESP_LOGI(TAG, "[ * ] Not supported state %d", el_state);
                       }
#endif
                   } else if ((int) msg.data == get_input_set_id()) {
                       ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                       ESP_LOGI(TAG, "[ * ] Stopping audio pipeline");
                       break;
                   } else if ((int) msg.data == get_input_volup_id()) {
                       ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                       player_volume += 10;
                       if (player_volume > 100) {
                           player_volume = 100;
                       }
                       audio_hal_set_volume(board_handle->audio_hal, player_volume);
                       ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                   } else if ((int) msg.data == get_input_voldown_id()) {
                       ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                       player_volume -= 10;
                       if (player_volume < 0) {
                           player_volume = 0;
                       }
                       audio_hal_set_volume(board_handle->audio_hal, player_volume);
                       ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                   }
       }
    }

    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);


}


