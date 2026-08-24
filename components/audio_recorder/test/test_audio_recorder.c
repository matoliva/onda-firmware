#include <stdint.h>
#include <string.h>

#include "audio_recorder.h"
#include "unity.h"

static uint16_t read_u16_le(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
}

TEST_CASE("audio recorder serializes canonical PCM WAV headers", "[audio_recorder]")
{
    uint8_t header[AUDIO_RECORDER_WAV_HEADER_SIZE];

    audio_recorder_make_wav_header(header, 320000U);

    TEST_ASSERT_EQUAL_MEMORY("RIFF", &header[0], 4U);
    TEST_ASSERT_EQUAL_UINT32(320036U, read_u32_le(&header[4]));
    TEST_ASSERT_EQUAL_MEMORY("WAVEfmt ", &header[8], 8U);
    TEST_ASSERT_EQUAL_UINT32(16U, read_u32_le(&header[16]));
    TEST_ASSERT_EQUAL_UINT16(1U, read_u16_le(&header[20]));
    TEST_ASSERT_EQUAL_UINT16(1U, read_u16_le(&header[22]));
    TEST_ASSERT_EQUAL_UINT32(16000U, read_u32_le(&header[24]));
    TEST_ASSERT_EQUAL_UINT32(32000U, read_u32_le(&header[28]));
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16_le(&header[32]));
    TEST_ASSERT_EQUAL_UINT16(16U, read_u16_le(&header[34]));
    TEST_ASSERT_EQUAL_MEMORY("data", &header[36], 4U);
    TEST_ASSERT_EQUAL_UINT32(320000U, read_u32_le(&header[40]));
}

TEST_CASE("audio recorder downmixes stereo frames without int16 overflow", "[audio_recorder]")
{
    const int16_t input[] = {32767, 32767, -32768, -32768, 1000, -1000};
    int16_t output[3] = {0};

    const size_t output_bytes = audio_recorder_downmix_stereo(input,
                                                               sizeof(input),
                                                               output,
                                                               sizeof(output));

    TEST_ASSERT_EQUAL_UINT32(sizeof(output), output_bytes);
    TEST_ASSERT_EQUAL_INT16(32767, output[0]);
    TEST_ASSERT_EQUAL_INT16(-32768, output[1]);
    TEST_ASSERT_EQUAL_INT16(0, output[2]);
}

TEST_CASE("audio recorder ignores incomplete PCM frames", "[audio_recorder]")
{
    const int16_t input[] = {1000, 3000, 5000};
    int16_t output[2] = {0};

    TEST_ASSERT_EQUAL_UINT32(sizeof(int16_t),
                             audio_recorder_downmix_stereo(input,
                                                           sizeof(input),
                                                           output,
                                                           sizeof(output)));
    TEST_ASSERT_EQUAL_INT16(2000, output[0]);

    TEST_ASSERT_EQUAL_UINT32(0U,
                             audio_recorder_downmix_stereo(input, 0U, output, sizeof(output)));
}
