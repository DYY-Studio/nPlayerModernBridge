#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../bridge/npa_highbit_copy.h"

static uint16_t read_u16(const uint8_t *value)
{
    uint16_t result;
    memcpy(&result, value, sizeof(result));
    return result;
}

int main(void)
{
    const uint8_t source_y[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 0xee, 0xee,
        9, 10, 11, 12, 13, 14, 15, 16, 0xee, 0xee
    };
    const uint8_t source_uv[] = {
        21, 22, 23, 24, 25, 26, 27, 28, 0xee, 0xee
    };
    uint8_t destination_y[24] = {0};
    uint8_t destination_uv[20] = {0};

    assert(npa_copy_p010_planes(
        source_y, source_uv, 10, 10,
        destination_y, destination_uv, 12, 10,
        4, 2
    ) == 0);
    assert(memcmp(destination_y, (uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}, 8) == 0);
    assert(memcmp(destination_y + 12, (uint8_t[]){9, 10, 11, 12, 13, 14, 15, 16}, 8) == 0);
    assert(memcmp(destination_uv, (uint8_t[]){21, 22, 23, 24, 25, 26, 27, 28}, 8) == 0);
    assert(destination_y[8] == 0 && destination_uv[8] == 0);

    assert(npa_copy_p010_planes(
        source_y, source_uv, 7, 10,
        destination_y, destination_uv, 12, 10,
        4, 2
    ) != 0);

    const uint8_t source_y422[] = {
        0x23, 0x01, 0x34, 0x02, 0x45, 0x03, 0x56, 0x00, 0xee, 0xee,
        0x67, 0x01, 0x78, 0x02, 0x89, 0x03, 0x9a, 0x00, 0xee, 0xee,
    };
    const uint8_t source_u422[] = {
        0x11, 0x01, 0x22, 0x02, 0xee, 0xee,
        0x33, 0x01, 0x44, 0x02, 0xee, 0xee,
    };
    const uint8_t source_v422[] = {
        0x55, 0x02, 0x66, 0x03, 0xee, 0xee,
        0x77, 0x02, 0x88, 0x03, 0xee, 0xee,
    };
    uint8_t destination_y422[24] = {0};
    uint8_t destination_uv422[40] = {0};
    assert(npa_pack_yuvp10_to_biplanar(
        source_y422, source_u422, source_v422,
        10, 6, 6,
        destination_y422, destination_uv422, 12, 20,
        4, 2, 1, 0, 0
    ) == 0);
    assert(memcmp(destination_y422, (uint8_t[]){0xc0, 0x48, 0x00, 0x8d, 0x40, 0xd1, 0x80, 0x15}, 8) == 0);
    assert(memcmp(destination_y422 + 12, (uint8_t[]){0xc0, 0x59, 0x00, 0x9e, 0x40, 0xe2, 0x80, 0x26}, 8) == 0);
    assert(memcmp(destination_uv422, (uint8_t[]){0x40, 0x44, 0x40, 0x95, 0x80, 0x88, 0x80, 0xd9}, 8) == 0);
    assert(memcmp(destination_uv422 + 20, (uint8_t[]){0xc0, 0x4c, 0xc0, 0x9d, 0x00, 0x91, 0x00, 0xe2}, 8) == 0);
    assert(destination_uv422[8] == 0 && destination_uv422[20 + 8] == 0);

    const uint8_t source_y444[] = {
        0x01, 0x00, 0x02, 0x00, 0xee, 0xee,
        0x03, 0x00, 0x04, 0x00, 0xee, 0xee,
    };
    const uint8_t source_u444[] = {
        0x05, 0x00, 0x06, 0x00, 0xee, 0xee,
        0x07, 0x00, 0x08, 0x00, 0xee, 0xee,
    };
    const uint8_t source_v444[] = {
        0x09, 0x00, 0x0a, 0x00, 0xee, 0xee,
        0x0b, 0x00, 0x0c, 0x00, 0xee, 0xee,
    };
    uint8_t destination_y444[12] = {0};
    uint8_t destination_uv444[24] = {0};
    assert(npa_pack_yuvp10_to_biplanar(
        source_y444, source_u444, source_v444,
        6, 6, 6,
        destination_y444, destination_uv444, 6, 12,
        2, 2, 0, 0, 0
    ) == 0);
    assert(memcmp(destination_uv444, (uint8_t[]){0x40, 0x01, 0x40, 0x02, 0x80, 0x01, 0x80, 0x02}, 8) == 0);
    assert(memcmp(destination_uv444 + 12, (uint8_t[]){0xc0, 0x01, 0xc0, 0x02, 0x00, 0x02, 0x00, 0x03}, 8) == 0);
    assert(destination_uv444[8] == 0 && destination_uv444[20] == 0);

    const uint8_t source_y420[] = {0x00, 0x00, 0x00, 0x02, 0xff, 0x03, 0x00, 0x00};
    const uint8_t source_u420[] = {0x00, 0x00};
    const uint8_t source_v420[] = {0x00, 0x02};
    uint8_t destination_y420[8] = {0};
    uint8_t destination_uv420[4] = {0};
    assert(npa_pack_yuv420p10_to_p010(
        source_y420, source_u420, source_v420,
        4, 2, 2,
        destination_y420, destination_uv420, 4, 4,
        2, 2, 1
    ) == 0);
    assert(memcmp(destination_y420, (uint8_t[]){0x00, 0x10, 0x80, 0x7d, 0x00, 0xeb, 0x00, 0x10}, 8) == 0);
    assert(memcmp(destination_uv420, (uint8_t[]){0x00, 0x10, 0x00, 0x80}, 4) == 0);

    /* 12-bit 4:2:0 -> SV22: 12-to-16 alignment and row-copy upsampling. */
    const uint16_t source_y12[] = {
        0x0100, 0x0eb0, 0x0fff, 0xaaaa,
        0x0000, 0x0800, 0x0001, 0xaaaa,
        0x0123, 0x0456, 0x0789, 0xaaaa,
    };
    const uint16_t source_u12[] = {0x0100, 0x0800, 0xaaaa, 0xaaaa, 0x0fff, 0x0aaa};
    const uint16_t source_v12[] = {0x0200, 0x0900, 0xaaaa, 0xaaaa, 0x0eee, 0x0aaa};
    uint8_t destination_y12[24] = {0};
    uint8_t destination_uv12[48] = {0};
    assert(npa_pack_yuvp16_to_biplanar16(
        (const uint8_t *)source_y12, (const uint8_t *)source_u12, (const uint8_t *)source_v12,
        8, 8, 8,
        destination_y12, destination_uv12, 8, 16,
        3, 3, 1, 1, 12, 0
    ) == 0);
    assert(read_u16(destination_y12) == 0x1000);
    assert(read_u16(destination_y12 + 2) == 0xeb00);
    assert(read_u16(destination_y12 + 4) == 0xfff0);
    assert(read_u16(destination_uv12) == 0x1000);
    assert(read_u16(destination_uv12 + 2) == 0x2000);
    assert(read_u16(destination_uv12 + 4) == 0x8000);
    assert(read_u16(destination_uv12 + 6) == 0x9000);
    assert(memcmp(destination_uv12, destination_uv12 + 16, 8) == 0);
    assert(memcmp(destination_uv12 + 32, (uint8_t[]){0xf0, 0xff, 0xe0, 0xee, 0xa0, 0xaa, 0xa0, 0xaa}, 8) == 0);
    assert(destination_uv12[8] == 0 && destination_uv12[16 + 8] == 0);

    /* Full-range 12-bit input is explicitly remapped to SV22's video range. */
    const uint16_t full_y12[] = {0, 4095, 2048, 1024};
    const uint16_t full_u12[] = {0, 2048};
    const uint16_t full_v12[] = {4095, 0};
    uint8_t video_y16[8] = {0};
    uint8_t video_uv16[8] = {0};
    assert(npa_pack_yuvp16_to_biplanar16(
        (const uint8_t *)full_y12, (const uint8_t *)full_u12, (const uint8_t *)full_v12,
        8, 4, 4,
        video_y16, video_uv16, 8, 8,
        4, 1, 1, 0, 12, 1
    ) == 0);
    assert(read_u16(video_y16) == 4096 && read_u16(video_y16 + 2) == 60160);
    assert(read_u16(video_uv16) == 4096 && read_u16(video_uv16 + 2) == 61440);
    assert(read_u16(video_uv16 + 4) == 32768 && read_u16(video_uv16 + 6) == 4096);

    const uint16_t full_y16[] = {0, 65535, 32768, 1};
    const uint16_t full_u16[] = {0, 32768};
    const uint16_t full_v16[] = {65535, 0};
    uint8_t video_y16_from_full[8] = {0};
    uint8_t video_uv16_from_full[8] = {0};
    assert(npa_pack_yuvp16_to_biplanar16(
        (const uint8_t *)full_y16, (const uint8_t *)full_u16, (const uint8_t *)full_v16,
        8, 4, 4,
        video_y16_from_full, video_uv16_from_full, 8, 8,
        4, 1, 1, 0, 16, 1
    ) == 0);
    assert(read_u16(video_y16_from_full) == 4096 && read_u16(video_y16_from_full + 2) == 60160);
    assert(read_u16(video_uv16_from_full) == 4096 && read_u16(video_uv16_from_full + 2) == 61440);
    assert(read_u16(video_uv16_from_full + 4) == 32768 && read_u16(video_uv16_from_full + 6) == 4096);

    /* 16-bit limited-range legal code values are preserved; 444 remains sv44. */
    const uint16_t source_y16[] = {4096, 60160, 12345, 54321};
    const uint16_t source_u16[] = {4096, 32768, 61440, 32123};
    const uint16_t source_v16[] = {61440, 32768, 4096, 45678};
    uint8_t destination_y16[8] = {0};
    uint8_t destination_uv16[16] = {0};
    assert(npa_pack_yuvp16_to_biplanar16(
        (const uint8_t *)source_y16, (const uint8_t *)source_u16, (const uint8_t *)source_v16,
        4, 4, 4,
        destination_y16, destination_uv16, 4, 8,
        2, 2, 0, 0, 16, 0
    ) == 0);
    assert(memcmp(destination_y16, source_y16, sizeof(source_y16)) == 0);
    assert(read_u16(destination_uv16) == 4096 && read_u16(destination_uv16 + 2) == 61440);
    assert(read_u16(destination_uv16 + 4) == 32768 && read_u16(destination_uv16 + 6) == 32768);
    assert(read_u16(destination_uv16 + 8) == 61440 && read_u16(destination_uv16 + 10) == 4096);

    return 0;
}
