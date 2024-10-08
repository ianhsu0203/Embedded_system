#include <events/mbed_events.h>
#include <stdio.h>
#include <stdlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include "arm_math.h"

#include "CircularBuffer.h"
#include "SensorService.h"
#include "arm_math.h"
#include "ble/BLE.h"
#include "ble/Gap.h"
#include "math_helper.h"
#include "mbed-trace/mbed_trace.h"
#include "mbed.h"
#include "pretty_printer.h"
#include "stm32l475e_iot01_gyro.h"
#include "stm32l475e_iot01_accelero.h"
#include "stm32l475e_iot01_magneto.h"


using namespace std::literals::chrono_literals;

const static char DEVICE_NAME[] = "SENSOR_DEVICE";

// Filter related constants
#define _FILTER_
#define _AVG_
#define NUM_TAPS 29
#define BLOCK_SIZE 32
static float32_t firStateF32[BLOCK_SIZE + NUM_TAPS - 1];
static const float32_t firCoeffs32[NUM_TAPS] = {
    -0.0018225230f, -0.0015879294f, +0.0000000000f, +0.0036977508f, +0.0080754303f, +0.0085302217f,
    -0.0000000000f, -0.0173976984f, -0.0341458607f, -0.0333591565f, +0.0000000000f, +0.0676308395f,
    +0.1522061835f, +0.2229246956f, +0.2504960933f, +0.2229246956f, +0.1522061835f, +0.0676308395f,
    +0.0000000000f, -0.0333591565f, -0.0341458607f, -0.0173976984f, -0.0000000000f, +0.0085302217f,
    +0.0080754303f, +0.0036977508f, +0.0000000000f, -0.0015879294f, -0.0018225230f};

arm_fir_instance_f32 S;

static events::EventQueue event_queue(/* event count */ 16 * EVENTS_EVENT_SIZE);
InterruptIn button(BUTTON1);

class SensorDemo : ble::Gap::EventHandler {
   public:
    SensorDemo(BLE &ble, events::EventQueue &event_queue)
        : _ble(ble),
          _event_queue(event_queue),
          _sensor_uuid(CUSTOM_SENSOR_SERVICE_UUID),
          _sensor_service(ble),
          _adv_data_builder(_adv_buffer) {
        arm_fir_init_f32(&S, NUM_TAPS, (float32_t *)&firCoeffs32[0], &firStateF32[0], BLOCK_SIZE);
    }

    void start() {
        _ble.init(this, &SensorDemo::on_init_complete);

        _event_queue.dispatch_forever();
    }

   private:
    /** Callback triggered when the ble initialization process has finished */
    void on_init_complete(BLE::InitializationCompleteCallbackContext *params) {
        if (params->error != BLE_ERROR_NONE) {
            printf("Ble initialization failed.\r\n");
            return;
        }

        print_mac_address();
        BSP_ACCELERO_Init();
        BSP_MAGNETO_Init();
        BSP_GYRO_Init();

        button.fall(Callback<void()>(this, &SensorDemo::button_pressed));
        button.rise(Callback<void()>(this, &SensorDemo::button_released));
        /* This allows us to receive events like onConnectionComplete() */
        _ble.gap().setEventHandler(this);

        /* Sensor value updated every second */
        _event_queue.call_every(  // update
            50ms, [this] { update_sensor_value(); });

        _event_queue.call_every(  // gather
            1ms, [this] { gather_data(); });

        start_advertising();
    }

    void start_advertising() {
        /* Create advertising parameters and payload */

        ble::AdvertisingParameters adv_parameters(ble::advertising_type_t::CONNECTABLE_UNDIRECTED,
                                                  ble::adv_interval_t(ble::millisecond_t(100)));

        _adv_data_builder.setFlags();
        _adv_data_builder.setAppearance(ble::adv_data_appearance_t::GENERIC_HEART_RATE_SENSOR);
        _adv_data_builder.setLocalServiceList({&_sensor_uuid, 1});
        _adv_data_builder.setName(DEVICE_NAME);

        /* Setup advertising */

        ble_error_t error =
            _ble.gap().setAdvertisingParameters(ble::LEGACY_ADVERTISING_HANDLE, adv_parameters);

        if (error) {
            printf("_ble.gap().setAdvertisingParameters() failed\r\n");
            return;
        }

        error = _ble.gap().setAdvertisingPayload(ble::LEGACY_ADVERTISING_HANDLE,
                                                 _adv_data_builder.getAdvertisingData());

        if (error) {
            printf("_ble.gap().setAdvertisingPayload() failed\r\n");
            return;
        }

        /* Start advertising */

        error = _ble.gap().startAdvertising(ble::LEGACY_ADVERTISING_HANDLE);

        if (error) {
            printf("_ble.gap().startAdvertising() failed\r\n");
            return;
        }

        printf("Sensor service advertising, please connect\r\n");
    }
    float32_t average(float32_t *data, int size) {
        float32_t sum = 0;
        for (int i = 0; i < size; i++) {
            sum += data[i];
        }
        return sum / size;
    }
    float32_t processValue(Circular_Buffer<int16_t, BLOCK_SIZE * 2> buffer){
        float32_t filteredData[BLOCK_SIZE];
        float32_t storeBuffer[BLOCK_SIZE];
        int16_t tmp;
        /**********************Process X Data************************/
        for (int i = 0; i < BLOCK_SIZE; i++) {
            if (!buffer.get(tmp)) tmp = 0;
            storeBuffer[i] = float32_t(tmp);
        }
        #ifdef _FILTER_
        arm_fir_f32(&S, &storeBuffer[0], &filteredData[0], BLOCK_SIZE);
            #ifdef _AVG_
            return average(&filteredData[0], BLOCK_SIZE);
            #else
            return filteredData[0];
            #endif
        #else
            #ifdef _AVG_
            return average(&storeBuffer[0], BLOCK_SIZE);
            #else
            return storeBuffer[0];
            #endif
        #endif
    }
    void update_sensor_value()  // pass through the filter and then update every 50ms
    {
        
        /**********************Process ACCEROL************************/
        _GyroDataXYZ.x = processValue(_circular_buffer_x);
        _GyroDataXYZ.y = processValue(_circular_buffer_y);
        _GyroDataXYZ.z = processValue(_circular_buffer_z);
        /**********************Process MAGENATO************************/
        _GyroDataXYZ.mx = processValue(_circular_buffer_mx);
        _GyroDataXYZ.my = processValue(_circular_buffer_my);
        _GyroDataXYZ.mz = processValue(_circular_buffer_mz);
        /************************************************************/
        _sensor_service.updateGyroDataXYZ(_GyroDataXYZ);
    }
    /*********************Button***********************/
    void write_command(){
        _sensor_service.write(1);
    }
    void button_pressed() {}
    void button_released() {
        _event_queue.call(Callback<void()>(this, &SensorDemo::write_command));
    }
    /**************************************************/
    void gather_data() {  // gather data every 1ms
        BSP_ACCELERO_AccGetXYZ(sensorData);
        // BSP_GYRO_GetXYZ(sensorData);
        _circular_buffer_x.put(sensorData[0]);  // put x-value into buffer
        _circular_buffer_y.put(sensorData[1]);  // put y-value into buffer
        _circular_buffer_z.put(sensorData[2]);  // put z-value into buffer
        //_circular_buffer.put(sensorData); //put 3-axis simultaneously
        BSP_MAGNETO_GetXYZ(sensorData);
        _circular_buffer_mx.put(sensorData[0]);  // put x-value into buffer
        _circular_buffer_my.put(sensorData[1]);  // put y-value into buffer
        _circular_buffer_mz.put(sensorData[2]);  // put z-value into buffer
    }

    /* These implement ble::Gap::EventHandler */
   private:
    virtual void onConnectionComplete(const ble::ConnectionCompleteEvent &event) {
        if (event.getStatus() == ble_error_t::BLE_ERROR_NONE) {
            printf("Client connected, you may now subscribe to updates\r\n");
            printf("Device Name: %s\r\n", DEVICE_NAME);
        }
    }

    virtual void onDisconnectionComplete(const ble::DisconnectionCompleteEvent &event) {
        printf("Client disconnected, restarting advertising\r\n");

        ble_error_t error = _ble.gap().startAdvertising(ble::LEGACY_ADVERTISING_HANDLE);

        if (error) {
            printf("_ble.gap().startAdvertising() failed\r\n");
            return;
        }
    }

   private:
    BLE &_ble;
    events::EventQueue &_event_queue;

    UUID _sensor_uuid;

    // float sensorData[3];  // data need to be put into buffer
    int16_t sensorData[3];  // data need to be put into buffer

    // if only send x-value
    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_x;  // store x-value
    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_y;  // store y-value
    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_z;  // store z-value

    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_mx;  // store x-value of magenato
    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_my;  // store y-value of magenato
    Circular_Buffer<int16_t, BLOCK_SIZE * 2> _circular_buffer_mz;  // store z-value of magenato
    SensorService::GyroType_t _GyroDataXYZ;  // struct
    SensorService _sensor_service;

    uint8_t _adv_buffer[ble::LEGACY_ADVERTISING_MAX_SIZE];
    ble::AdvertisingDataBuilder _adv_data_builder;
};

/* Schedule processing of events from the BLE middleware in the event queue. */
void schedule_ble_events(BLE::OnEventsToProcessCallbackContext *context) {
    event_queue.call(Callback<void()>(&context->ble, &BLE::processEvents));
}

int main() {
    mbed_trace_init();
    BSP_GYRO_Init();

    BLE &ble = BLE::Instance();
    ble.onEventsToProcess(schedule_ble_events);

    SensorDemo demo(ble, event_queue);
    demo.start();

    return 0;
}
