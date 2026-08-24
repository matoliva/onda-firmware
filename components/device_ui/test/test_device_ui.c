#include <string.h>

#include "device_ui.h"
#include "unity.h"

static device_ui_state_t make_state(device_ui_primary_state_t primary_state)
{
    return (device_ui_state_t){
        .primary_state = primary_state,
        .wifi_status = DEVICE_UI_WIFI_CONNECTED,
        .storage_status = DEVICE_UI_STORAGE_AVAILABLE,
        .battery_status = DEVICE_UI_BATTERY_UNKNOWN,
        .proof_of_possession = "AB12CD34",
        .saved_duration = "00:42",
        .saved_filename = "recording-0001.wav",
    };
}

TEST_CASE("device UI maps primary states to reusable screens", "[device_ui]")
{
    const struct {
        device_ui_primary_state_t primary_state;
        const char *title;
        display_color_t title_color;
    } cases[] = {
        {DEVICE_UI_PRIMARY_READY, "Ready", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_RECORDING, "Recording", DISPLAY_COLOR_RED},
        {DEVICE_UI_PRIMARY_FINALIZING, "Saving", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SAVED, "Saved", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_WIFI_SETUP, "Wi-Fi setup", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_STORAGE_ERROR, "NO SD CARD", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_AUDIO_ERROR, "AUDIO ERROR", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_ERROR, "Something went", DISPLAY_COLOR_BLACK},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const device_ui_state_t state = make_state(cases[index].primary_state);
        display_screen_t screen;

        TEST_ASSERT_EQUAL(ESP_OK, device_ui_describe(&state, &screen));
        TEST_ASSERT_EQUAL_STRING(cases[index].title, screen.title);
        TEST_ASSERT_EQUAL(cases[index].title_color, screen.title_color);
    }
}

TEST_CASE("device UI maps all storage indicator states", "[device_ui]")
{
    const struct {
        device_ui_storage_status_t input;
        display_storage_status_t output;
    } cases[] = {
        {DEVICE_UI_STORAGE_AVAILABLE, DISPLAY_STORAGE_AVAILABLE},
        {DEVICE_UI_STORAGE_UNAVAILABLE, DISPLAY_STORAGE_UNAVAILABLE},
        {DEVICE_UI_STORAGE_ERROR, DISPLAY_STORAGE_ERROR},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        device_ui_state_t state = make_state(DEVICE_UI_PRIMARY_READY);
        display_screen_t screen;
        state.storage_status = cases[index].input;

        TEST_ASSERT_EQUAL(ESP_OK, device_ui_describe(&state, &screen));
        TEST_ASSERT_EQUAL(cases[index].output, screen.storage_status);
    }
}

TEST_CASE("device UI refreshes only primary state transitions", "[device_ui]")
{
    device_ui_state_t previous = make_state(DEVICE_UI_PRIMARY_READY);
    device_ui_state_t next = previous;

    TEST_ASSERT_TRUE(device_ui_should_refresh(NULL, &next));

    next.wifi_status = DEVICE_UI_WIFI_OFFLINE;
    next.storage_status = DEVICE_UI_STORAGE_ERROR;
    TEST_ASSERT_TRUE(device_ui_should_refresh(&previous, &next));

    previous.primary_state = DEVICE_UI_PRIMARY_RECORDING;
    next.primary_state = DEVICE_UI_PRIMARY_RECORDING;
    TEST_ASSERT_FALSE(device_ui_should_refresh(&previous, &next));

    next.primary_state = DEVICE_UI_PRIMARY_RECORDING;
    previous.primary_state = DEVICE_UI_PRIMARY_READY;
    TEST_ASSERT_TRUE(device_ui_should_refresh(&previous, &next));
    TEST_ASSERT_FALSE(device_ui_should_refresh(&previous, NULL));
}

TEST_CASE("Saved UI requires duration and filename", "[device_ui]")
{
    device_ui_state_t state = make_state(DEVICE_UI_PRIMARY_SAVED);
    display_screen_t screen;

    memset(state.saved_filename, 0, sizeof(state.saved_filename));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, device_ui_describe(&state, &screen));
}

TEST_CASE("Wi-Fi setup requires a proof of possession", "[device_ui]")
{
    device_ui_state_t state = make_state(DEVICE_UI_PRIMARY_WIFI_SETUP);
    display_screen_t screen;

    memset(state.proof_of_possession, 0, sizeof(state.proof_of_possession));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, device_ui_describe(&state, &screen));
}
