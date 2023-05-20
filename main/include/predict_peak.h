#ifndef PREDICT_PEAK_H
#define PREDICT_PEAK_H

#include <stdint.h>
#include <time.h>

#define PREDICT_PEAK_NVS_NAMESPACE "predict_peak"
#define PREDICT_PEAK_NVS_KEY_METHOD "method"    // Predict peak method (enum predict_peak_method_e)
#define PREDICT_PEAK_TASK_INTERVAL_MS 5000

enum predict_peak_method_e {
    PREDICT_PEAK_METHOD_LINEAR_REGRESSION = 0,
    PREDICT_PEAK_METHOD_WEIGHTED_AVERAGE = 1
};

struct predicted_peak_s {
    float value;
    time_t timestamp;
};


// Function prototypes
_Noreturn void predict_peak_task(void *pvParameters);
SemaphoreHandle_t predict_peak_get_predicted_peak_mutex_handle(void);
struct predicted_peak_s predict_peak_get_predicted_peak(void);

#endif //PREDICT_PEAK_H
