#include "Particle.h"
#include <lvgl.h>
#include <Adafruit_EPD_RK.h>
#include "JsonParserGeneratorRK.h"
#include "LocalTimeRK.h"

// Let Device OS manage the connection to the Particle Cloud
SYSTEM_MODE(AUTOMATIC);

// Show system, cloud connectivity, and application logs over USB
// View logs with CLI using 'particle serial monitor --follow'
SerialLogHandler logHandler(
    LOG_LEVEL_NONE, // Default logging level for all categories
    {
        {"app", LOG_LEVEL_ALL} // Only enable all log levels for the application
    });

#define SD_CS D2
#define SRAM_CS D3
#define EPD_CS D4
#define EPD_DC D5

#define EPD_RESET -1 // can set to -1 and share with microcontroller Reset!
#define EPD_BUSY -1  // can set to -1 to not use a pin (will wait a fixed delay)

/*Set to your screen resolution and rotation*/
#define HOR_RES 250
#define VER_RES 122
#define ROTATION LV_DISPLAY_ROTATION_0

#define X_OFFSET 40
#define Y_OFFSET 25

#define PADDING 5

#define NUM_FORECAST_ENTRIES 4

const size_t JSON_BUFFER_SIZE = 4096;
const size_t MAX_TOKENS = 2048;

Ledger deviceConfig;

JsonParserStatic<JSON_BUFFER_SIZE, MAX_TOKENS> jsonParser;

CloudEvent event;

float maxTemperature = 0.0;
float minTemperature = 1000.0;

bool didUpdateScreen = false;
bool didPublish = false;
bool didSync = false;

double latitude = 0.0;
double longitude = 0.0;
String localTimePosixTz = "";

Adafruit_SSD1680 epd(HOR_RES, VER_RES, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);

/*LVGL draw into this buffer, 1/10 screen size usually works well. The size is in bytes*/
#define DRAW_BUF_SIZE (HOR_RES * VER_RES / 5 * (LV_COLOR_DEPTH))

uint32_t draw_buf[DRAW_BUF_SIZE / 4];

struct ForecastEntry
{
    String dt_txt; // Changed to String to handle dynamic allocation
    float temp;
    int precip;
};

ForecastEntry forecastList[NUM_FORECAST_ENTRIES];

void my_print(lv_log_level_t level, const char *buf)
{
    LV_UNUSED(level);
    Log.info(buf);
}

/* LVGL calls it when a rendered image needs to copied to the display*/
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, unsigned char *px_map)
{
    epd.clearBuffer();

    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    Log.info("w: %ld, h: %ld", w, h);

    // Invert the pixel map for eink display
    uint32_t px_map_size = w * h / 8;
    unsigned char inverted_px_map[px_map_size];
    for (uint32_t i = 0; i < px_map_size; i++)
        inverted_px_map[i] = ~px_map[i + 8]; // Skip the 8-byte header

    epd.drawBitmap(area->x1, area->y1, (uint8_t *)inverted_px_map, w, h, EPD_BLACK);
    epd.display();
    lv_display_flush_ready(disp);
}

/*use millis() as tick source*/
static uint32_t my_tick(void)
{
    return millis();
}

void drawWeatherForecast()
{
    lv_obj_t *screen = lv_scr_act();

    lv_obj_clean(screen);

    int chartWidth = HOR_RES - X_OFFSET - PADDING;
    int chartHeight = VER_RES - Y_OFFSET - PADDING;

    // Create temperature chart
    lv_obj_t *temp_chart = lv_chart_create(screen);
    lv_obj_set_size(temp_chart, chartWidth, chartHeight);
    lv_obj_align(temp_chart, LV_ALIGN_BOTTOM_LEFT, X_OFFSET, -Y_OFFSET);

    lv_chart_set_type(temp_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(temp_chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_range(temp_chart, LV_CHART_AXIS_PRIMARY_Y, minTemperature, maxTemperature);
    lv_chart_set_point_count(temp_chart, NUM_FORECAST_ENTRIES);

    // Add padding settings for alignment
    lv_obj_set_style_pad_all(temp_chart, 0, 0);
    lv_chart_set_div_line_count(temp_chart, 0, 0);

    lv_chart_series_t *temp_series = lv_chart_add_series(temp_chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);

    // Create precipitation chart
    lv_obj_t *precip_chart = lv_chart_create(screen);
    lv_obj_set_size(precip_chart, chartWidth, chartHeight); // Match temp chart size
    lv_obj_align(precip_chart, LV_ALIGN_BOTTOM_LEFT, X_OFFSET, -Y_OFFSET);

    lv_chart_set_type(precip_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_range(precip_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(precip_chart, NUM_FORECAST_ENTRIES);

    lv_obj_set_style_bg_opa(precip_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(precip_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(precip_chart, 0, 0);
    lv_chart_set_div_line_count(precip_chart, 0, 0);

    // Use gray color for precipitation bars
    lv_chart_series_t *precip_series = lv_chart_add_series(precip_chart, lv_color_hex(0x606060), LV_CHART_AXIS_PRIMARY_Y);

    // Set temperature and precipitation points
    for (int i = 0; i < NUM_FORECAST_ENTRIES; i++)
    {
        lv_chart_set_value_by_id(temp_chart, temp_series, i, (int)forecastList[i].temp);
        lv_chart_set_value_by_id(precip_chart, precip_series, i, (int)forecastList[i].precip);

        lv_obj_t *label_x = lv_label_create(screen);
        lv_label_set_text(label_x, forecastList[i].dt_txt.c_str());
        // Align the label to the bottom left of the chart, multiplier determined by trial and error
        lv_obj_align(label_x, LV_ALIGN_BOTTOM_LEFT, (X_OFFSET + 5) + (i * 54), 0);
    }

    lv_obj_set_style_opa(precip_chart, LV_OPA_50, LV_PART_ITEMS);

    lv_chart_refresh(temp_chart);
    lv_chart_refresh(precip_chart);

    // Create label for maximum temperature
    char maxTempBuf[8];
    snprintf(maxTempBuf, sizeof(maxTempBuf), "%.0f°F", maxTemperature);

    lv_obj_t *label_y_end = lv_label_create(screen);
    lv_label_set_text(label_y_end, maxTempBuf);
    lv_obj_align(label_y_end, LV_ALIGN_TOP_LEFT, PADDING, PADDING);

    // Create label for minimum temperature
    char minTempBuf[8];
    snprintf(minTempBuf, sizeof(minTempBuf), "%.0f°F", minTemperature);

    lv_obj_t *label_y_start = lv_label_create(screen);
    lv_label_set_text(label_y_start, minTempBuf);
    lv_obj_align(label_y_start, LV_ALIGN_BOTTOM_LEFT, PADDING, -Y_OFFSET);

    lv_refr_now(NULL); // Eink refresh
    didUpdateScreen = true;
}

void handleWeatherResponse(const char *event, const char *data)
{
    if (!jsonParser.addChunkedData(event, data))
    {
        Log.error("Failed to add chunked data, might need to allocate more space for data");
        return;
    }
    if (!jsonParser.parse())
    {
        // Parsing failed, likely due to an incomplete response, wait for more chunks
        return;
    }

    JsonReference root = jsonParser.getReference();
    JsonReference apiList = root.key("list");

    int count = apiList.size();

    // Populate forecastList with API data
    for (int i = 0; i < count && i < NUM_FORECAST_ENTRIES; i++)
    {
        JsonReference entry = apiList.index(i);

        float temp = entry.key("main").key("temp").valueFloat();
        float pop = entry.key("pop").valueFloat() * 100;
        time_t epoch = entry.key("dt").valueUnsignedLong();

        LocalTimeConvert conv;
        conv.withTime(epoch).convert();
        String formatted = conv.format("%I %p");

        if (formatted.startsWith("0"))
            formatted.remove(0, 1);

        // Populate forecast entry
        forecastList[i].temp = temp;
        forecastList[i].precip = (int)pop;
        forecastList[i].dt_txt = formatted;

        Log.info("Entry %d: Temp: %.1f F, Precip: %d%%, Time: %s",
                 i, temp, (int)pop, formatted.c_str());

        if (temp > maxTemperature)
            maxTemperature = temp;
        if (temp < minTemperature)
            minTemperature = temp;
    }

    drawWeatherForecast(); // Redraw the forecast
}

void ledgerSyncCallback(Ledger ledger, void *)
{
    Log.info("Ledger %s synchronized at %llu", ledger.name(), ledger.lastSynced());
    LedgerData config = ledger.get();

    latitude = config["lat"].asDouble();
    longitude = config["lon"].asDouble();
    localTimePosixTz = config["posix_tz"].asString();

    Log.info("Latitude: %f, Longitude: %f", latitude, longitude);
    Log.info("Timezone: %s", localTimePosixTz.c_str());

    // Set timezone to the Eastern United States
    LocalTime::instance().withConfig(LocalTimePosixTimezone(localTimePosixTz));
    didSync = true;
}

void setup()
{
    lv_init();
    lv_tick_set_cb(my_tick);
    lv_log_register_print_cb(my_print);

    lv_display_t *disp;
    disp = lv_display_create(HOR_RES, VER_RES);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    Log.info("Starting display...");
    epd.begin();
    epd.clearBuffer();

    String topic = Particle.deviceID() + "/hook-response/weather";
    Particle.subscribe(topic, handleWeatherResponse);

    deviceConfig = Particle.ledger("photon2-c2d");
    deviceConfig.onSync(ledgerSyncCallback);
    ledgerSyncCallback(deviceConfig, nullptr);
}

void loop()
{
    if (Particle.connected() && didSync)
    {
        // https://docs.particle.io/reference/device-os/api/battery-voltage/battery-voltage-photon-2/

        if (didUpdateScreen)
        {
            Log.info("Going to sleep for 60 minutes...");
            SystemSleepConfiguration config;
            config.mode(SystemSleepMode::HIBERNATE)
                .duration(60min);
            System.sleep(config);
            Log.info("Woke up from sleep");
        }
        if (!didPublish)
        {
            Variant obj;
            obj.set("lat", latitude);
            obj.set("lon", longitude);
            obj.set("cnt", NUM_FORECAST_ENTRIES);

            event.name("weather");
            event.data(obj);

            Log.info("Publishing event...");

            // We'll set the didUpdateScreen flag on the callback function to the webhook
            Particle.publish(event);
            waitForNot(event.isSending, 60000);

            if (event.isSent())
            {
                Log.info("publish succeeded");
                event.clear();
                // Don't need to clear the flag because a hibernate will reset the device
                didPublish = true;
            }
            else if (!event.isOk())
            {
                Log.info("publish failed error=%d", event.error());
                event.clear();
            }
        }
    }
    lv_timer_handler();
    delay(5);
}