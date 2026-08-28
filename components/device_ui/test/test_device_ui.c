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
        {DEVICE_UI_PRIMARY_SYNCING, "SYNCING", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SYNCED, "SYNCED", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SYNC_PARTIAL, "SYNC PARTIAL", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_UP_TO_DATE, "UP TO DATE", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_NO_WIFI, "NO WIFI", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SYNC_AUTH_ERROR, "SYNC ERROR", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SYNC_CONNECTION_ERROR, "NO CONNECTION", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_WIFI_SETUP, "Wi-Fi setup", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_SLEEPING, "Sleeping", DISPLAY_COLOR_BLACK},
        {DEVICE_UI_PRIMARY_POWERING_OFF, "Powering off", DISPLAY_COLOR_BLACK},
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

TEST_CASE("Ready UI shows the complete button guide", "[device_ui]")
{
    const device_ui_state_t state = make_state(DEVICE_UI_PRIMARY_READY);
    display_screen_t screen;

    TEST_ASSERT_EQUAL(ESP_OK, device_ui_describe(&state, &screen));
    TEST_ASSERT_EQUAL_STRING("BOOT: rec / hold: sync", screen.detail_first_line);
    TEST_ASSERT_EQUAL_STRING("PWR: sleep / 2x: off", screen.detail_second_line);
    TEST_ASSERT_EQUAL_STRING("PWR 3s: Wi-Fi reset", screen.detail_third_line);
}

TEST_CASE("power transition UI describes the next physical action", "[device_ui]")
{
    const struct {
        device_ui_primary_state_t primary_state;
        const char *detail;
        display_color_t accent_color;
    } cases[] = {
        {DEVICE_UI_PRIMARY_SLEEPING, "Press PWR to wake", DISPLAY_COLOR_YELLOW},
        {DEVICE_UI_PRIMARY_POWERING_OFF, "Hold PWR to start", DISPLAY_COLOR_RED},
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const device_ui_state_t state = make_state(cases[index].primary_state);
        display_screen_t screen;

        TEST_ASSERT_EQUAL(ESP_OK, device_ui_describe(&state, &screen));
        TEST_ASSERT_EQUAL_STRING(cases[index].detail, screen.detail_first_line);
        TEST_ASSERT_EQUAL(cases[index].accent_color, screen.accent_color);
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

TEST_CASE("device UI maps all battery indicator states", "[device_ui]")
{
    const struct {
        device_ui_battery_status_t input;
        display_battery_status_t output;
    } cases[] = {
        {DEVICE_UI_BATTERY_UNKNOWN, DISPLAY_BATTERY_UNKNOWN},
        {DEVICE_UI_BATTERY_HIGH, DISPLAY_BATTERY_HIGH},
        {DEVICE_UI_BATTERY_MEDIUM, DISPLAY_BATTERY_MEDIUM},
        {DEVICE_UI_BATTERY_LOW, DISPLAY_BATTERY_LOW},
        {DEVICE_UI_BATTERY_CRITICAL, DISPLAY_BATTERY_CRITICAL},
    };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        device_ui_state_t state = make_state(DEVICE_UI_PRIMARY_READY);
        display_screen_t screen;
        state.battery_status = cases[index].input;

        TEST_ASSERT_EQUAL(ESP_OK, device_ui_describe(&state, &screen));
        TEST_ASSERT_EQUAL(cases[index].output, screen.battery_status);
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

TEST_CASE("device UI refreshes a stable battery transition outside recording", "[device_ui]")
{
    device_ui_state_t previous = make_state(DEVICE_UI_PRIMARY_READY);
    device_ui_state_t next = previous;

    next.battery_status = DEVICE_UI_BATTERY_LOW;
    TEST_ASSERT_TRUE(device_ui_should_refresh(&previous, &next));

    previous.primary_state = DEVICE_UI_PRIMARY_RECORDING;
    next.primary_state = DEVICE_UI_PRIMARY_RECORDING;
    TEST_ASSERT_FALSE(device_ui_should_refresh(&previous, &next));
}

TEST_CASE("device UI keeps pending power confirmations stable", "[device_ui]")
{
    device_ui_state_t previous = make_state(DEVICE_UI_PRIMARY_SLEEPING);
    device_ui_state_t next = previous;

    next.wifi_status = DEVICE_UI_WIFI_OFFLINE;
    next.storage_status = DEVICE_UI_STORAGE_ERROR;
    next.battery_status = DEVICE_UI_BATTERY_LOW;

    TEST_ASSERT_FALSE(device_ui_should_refresh(&previous, &next));

    next.primary_state = DEVICE_UI_PRIMARY_POWERING_OFF;
    TEST_ASSERT_TRUE(device_ui_should_refresh(&previous, &next));
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
