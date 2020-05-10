/*
  Copyright (c) 2019 Akshay Vernekar
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/
#include "include/app_httpd.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
//#include "camera_index.h"
#include "sdkconfig.h"
#include "include/app_camera.h"

#include "esp_log.h"
#include "image_util.h"
#include "include/app_tensorflow.h"
#include "freertos/queue.h"

static const char *TAG = "camera_httpd";

typedef struct
{
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

typedef struct
{
    size_t size;  //number of values used for filtering
    size_t index; //current value index
    size_t count; //value count
    int sum;
    int *values; //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size)
{
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if (!filter->values)
    {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value)
{
    if (!filter->values)
    {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size)
    {
        filter->count++;
    }
    return filter->sum / filter->count;
}

static esp_err_t prediction_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG,"a<--prediction_handler hitt!");
    esp_err_t res = ESP_OK;
    char* template = "\r\ndata: %s\r\n";
    char buffer[255] = {0}; 

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Content-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "Keep-Alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while(true)
    {
        const char* prediction = NULL;

        QueueHandle_t prediction_queue = tf_get_prediction_queue_handle();
        if(prediction_queue)
        {
            xQueueReceive(prediction_queue,&prediction,portMAX_DELAY);
            if(prediction != NULL)
            {
                ESP_LOGI(TAG,"Recieved prediction for index :%s",prediction);
                sprintf(buffer,template,prediction);
                res = httpd_resp_send_chunk(req, (const char *)buffer, strlen(buffer));

            }
        }

    }
    return res;
}


// static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
// {
//     jpg_chunking_t *j = (jpg_chunking_t *)arg;
//     if (!index)
//     {
//         j->len = 0;
//     }
//     if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK)
//     {
//         return 0;
//     }
//     j->len += len;
//     return len;
// }

// static esp_err_t capture_handler(httpd_req_t *req)
// {
//     camera_fb_t *fb = NULL;
//     esp_err_t res = ESP_OK;
//     int64_t fr_start = esp_timer_get_time();

//     if (!fb)
//     {
//         ESP_LOGE(TAG, "Camera capture failed");
//         httpd_resp_send_500(req);
//         return ESP_FAIL;
//     }

//     httpd_resp_set_type(req, "image/jpeg");
//     httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
//     httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

//     char ts[32];
//     snprintf(ts, 32, "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
//     httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

//     size_t fb_len = 0;
//     if (fb->format == PIXFORMAT_JPEG)
//     {
//         fb_len = fb->len;
//         res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
//     }
//     else
//     {
//         jpg_chunking_t jchunk = {req, 0};
//         res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
//         httpd_resp_send_chunk(req, NULL, 0);
//         fb_len = jchunk.len;
//     }
//     esp_camera_fb_return(fb);
//     int64_t fr_end = esp_timer_get_time();
//     ESP_LOGI(TAG, "JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
//     return res;
// }


static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    struct timeval _timestamp;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[128];

    static int64_t last_frame = 0;
    if (!last_frame)
    {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    //Start prediction task
    xTaskCreate(&tf_start_inference, "tf_start_inference", 1024*20, NULL, configMAX_PRIORITIES, NULL);

    while (true)
    {

        fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
        }
        else
        {
            _timestamp.tv_sec = fb->timestamp.tv_sec;
            _timestamp.tv_usec = fb->timestamp.tv_usec;
                if (fb->format != PIXFORMAT_JPEG)
                {
                    // int target_width = 28;
                    // int target_height = 28;
                    // int src_channel = 1;
                    // int buffer_size = target_width * target_height * src_channel;
                    // uint8_t* temp_buffer = (uint8_t*) malloc(buffer_size);

                    // ImageBuffer imageBuffer;
                    // imageBuffer.data = temp_buffer;
                    // imageBuffer.size = buffer_size;

                    // // --------------- Tensor flow part --------------------------
                    // // We need to resize our 96x96 image to 28x28 pixels as we have trained the model using this input size
                    // image_resize_linear(temp_buffer,fb->buf,target_width,target_height,src_channel,fb->width,fb->height);
                    // // The expected input has to be normalised for the values between 0-1
                    // xTaskCreate(&tf_perform_inference, "tf_perform_inference_task", 1024*20, &imageBuffer, configMAX_PRIORITIES, NULL);
                    // // --------------- Tensor flow part --------------------------

                    // // If you want to see 28x28 image uncomment this, but it will be small
                    // //bool jpeg_converted = fmt2jpg(temp_buffer, buffer_size, target_width, target_height, fb->format, 80, &_jpg_buf, &_jpg_buf_len);

                    // // free(temp_buffer);
                    // // temp_buffer = NULL;

                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if (!jpeg_converted)
                    {
                        ESP_LOGE(TAG, "JPEG compression failed");
                        res = ESP_FAIL;
                    }
                }
                else
                {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK)
        {
            size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf)
        {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK)
        {
            break;
        }
        int64_t fr_end = esp_timer_get_time();

        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
        ESP_LOGI(TAG, "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)"
                 ,
                 (uint32_t)(_jpg_buf_len),
                 (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
                 avg_frame_time, 1000.0 / avg_frame_time
        );
    }
    last_frame = 0;
    tf_stop_inference();
    return res;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    extern const unsigned char index_html_gz_start[] asm("_binary_index_html_gz_start");
    extern const unsigned char index_html_gz_end[] asm("_binary_index_html_gz_end");
    size_t index_ov2640_html_gz_len = index_html_gz_end - index_html_gz_start;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) 
    {
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_ov2640_html_gz_len);
    } else 
    {
        ESP_LOGE(TAG, "Camera sensor not found");
        return httpd_resp_send_500(req);
    }
}

void app_httpd_main()
{
    ESP_LOGI(TAG,"a<------sizeof(float)=%d",sizeof(float));
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL};

    httpd_uri_t prediction_uri = {
        .uri = "/prediction",
        .method = HTTP_GET,
        .handler = prediction_handler,
        .user_ctx = NULL};

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL};

    ra_filter_init(&ra_filter, 20);

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &prediction_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
