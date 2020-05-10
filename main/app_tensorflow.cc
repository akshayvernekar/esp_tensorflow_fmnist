#include "include/app_tensorflow.h"
#include "include/model_settings.h"
#include "include/model_data.h"

#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"
#include "tensorflow/lite/micro/kernels/all_ops_resolver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "include/app_camera.h"
#include "include/normalisation_lookup.h"
#include "image_util.h"

// Globals, used for compatibility with Arduino-style sketches.
namespace {
tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;

// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 73 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];
}  // namespace

// static SemaphoreHandle_t xMutex = NULL;
static bool isRunning = false;
static QueueHandle_t  predictionQueue =NULL;
// The name of this function is important for Arduino compatibility.
void tf_setup_init() 
{
	// Set up logging. Google style is to avoid globals or statics because of
	// lifetime uncertainty, but since this has a trivial destructor it's okay.
	// NOLINTNEXTLINE(runtime-global-variables)
	static tflite::MicroErrorReporter micro_error_reporter;
	error_reporter = &micro_error_reporter;

	// Map the model into a usable data structure. This doesn't involve any
	// copying or parsing, it's a very lightweight operation.
	model = tflite::GetModel(model_data_tflite);
	if (model->version() != TFLITE_SCHEMA_VERSION) 
	{
		TF_LITE_REPORT_ERROR(error_reporter,
		                     "Model provided is schema version %d not equal "
		                     "to supported version %d.",
		                     model->version(), TFLITE_SCHEMA_VERSION);
		return;
	}

	// Pull in only the operation implementations we need.
	// This relies on a complete list of all the ops needed by this graph.
	// An easier approach is to just use the AllOpsResolver, but this will
	// incur some penalty in code space for op implementations that are not
	// needed by this graph.
	//
	// tflite::ops::micro::AllOpsResolver resolver;
	// NOLINTNEXTLINE(runtime-global-variables)
	static tflite::ops::micro::AllOpsResolver micro_op_resolver;

	// Build an interpreter to run the model with.
	static tflite::MicroInterpreter static_interpreter(
	  model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
	interpreter = &static_interpreter;

	// Allocate memory from the tensor_arena for the model's tensors.
	TfLiteStatus allocate_status = interpreter->AllocateTensors();
	if (allocate_status != kTfLiteOk) 
	{
		TF_LITE_REPORT_ERROR(error_reporter, "AllocateTensors() failed");
		return;
	}

	// Get information about the memory area to use for the model's input.
	input = interpreter->input(0);

	// xMutex = xSemaphoreCreateMutex();
 //    if( xMutex != NULL )
 //        TF_LITE_REPORT_ERROR(error_reporter,"xMutex Semapore created succesfully!");

    isRunning = false;

    // This queue will hold the index of Max prediction
    predictionQueue = xQueueCreate(5, sizeof(const char*));
}

float* tf_get_input_pointer()
{
  return (interpreter->input(0))->data.f;
}


void normalise_image_buffer(float* dest_image_buffer,uint8* imageBuffer, int size )
{
  for (int i=0; i < size; i++)
  {
    //dest_image_buffer[i] = imageBuffer->data[i]/255.0f;
    dest_image_buffer[i] = get_normalised_value(imageBuffer[i]);
  }
}

void tf_start_inference(void* params)
{
    camera_fb_t *fb = NULL;

    if(isRunning)
    	goto exit;
    else
    	isRunning = true;


    while(isRunning)
    {
    	fb = esp_camera_fb_get();
        if (!fb)
        {
            TF_LITE_REPORT_ERROR(error_reporter, "Camera capture failed");
        }
        else
        {
			if (fb->format != PIXFORMAT_JPEG)
			{
				uint8_t* temp_buffer = (uint8_t*) malloc(kMaxImageSize);

				// --------------- Tensor flow part --------------------------
				// We need to resize our 96x96 image to 28x28 pixels as we have trained the model using this input size
				image_resize_linear(temp_buffer,fb->buf,kNumRows,kNumCols,kNumChannels,fb->width,fb->height);
                // The expected input has to be normalised for the values between 0-1
				normalise_image_buffer((interpreter->input(0))->data.f,temp_buffer,kMaxImageSize);

				// Free the memory initialised in httpd.c
				free(temp_buffer);

				// Run the model on this input and make sure it succeeds.
				if (kTfLiteOk != interpreter->Invoke()) 
				{
					TF_LITE_REPORT_ERROR(error_reporter, "Invoke failed.");
				}

				TfLiteTensor* output = interpreter->output(0);
				uint8 max_porb_index = 0;
				for (int i=1; i < kCategoryCount; i++)
				{
					if(output->data.f[i] > output->data.f[max_porb_index])
					{
						max_porb_index = i;
					}
				}

				if(predictionQueue)
				{
					xQueueSend(predictionQueue,(void*)&kCategoryLabels[max_porb_index],portMAX_DELAY);
				}
			}
		}
		esp_camera_fb_return(fb);
		fb = NULL;
		
        vTaskDelay(1000 / portTICK_RATE_MS);

    }

exit:    
	vTaskDelete(NULL);
}

void tf_stop_inference()
{
	isRunning = false;
}

QueueHandle_t tf_get_prediction_queue_handle()
{
	return predictionQueue;
}
