#include "npa_highbit_copy.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int npa_copy_p010_planes(
    const uint8_t *source_y,
    const uint8_t *source_uv,
    int source_y_stride,
    int source_uv_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height
)
{
    if (!source_y || !source_uv || !destination_y || !destination_uv
        || width <= 0 || height <= 0 || width == INT_MAX || height == INT_MAX) {
        return -1;
    }

    size_t y_row_bytes = (size_t)width * 2;
    size_t uv_row_bytes = ((size_t)width + 1) / 2 * 4;
    size_t uv_rows = ((size_t)height + 1) / 2;
    if (y_row_bytes > INT_MAX || uv_row_bytes > INT_MAX
        || source_y_stride < (int)y_row_bytes
        || destination_y_stride < (int)y_row_bytes
        || source_uv_stride < (int)uv_row_bytes
        || destination_uv_stride < (int)uv_row_bytes) {
        return -1;
    }

    for (int row = 0; row < height; row++) {
        memcpy(
            destination_y + (size_t)row * destination_y_stride,
            source_y + (size_t)row * source_y_stride,
            y_row_bytes
        );
    }
    for (size_t row = 0; row < uv_rows; row++) {
        memcpy(
            destination_uv + row * destination_uv_stride,
            source_uv + row * source_uv_stride,
            uv_row_bytes
        );
    }
    return 0;
}

static int npa_valid_plane_stride(int stride, size_t row_bytes)
{
    if (stride == INT_MIN) {
        return 0;
    }
    int magnitude = stride < 0 ? -stride : stride;
    return magnitude >= 0 && (size_t)magnitude >= row_bytes;
}

static uint16_t npa_load_u16(const uint8_t *source)
{
    uint16_t value;
    memcpy(&value, source, sizeof(value));
    return value & 0x03ff;
}

static uint16_t npa_full_to_video_luma(uint16_t value)
{
    return (uint16_t)(64 + ((uint32_t)value * 876 + 511) / 1023);
}

static uint16_t npa_full_to_video_chroma(uint16_t value)
{
    if (value <= 512) {
        return (uint16_t)(512 - ((uint32_t)(512 - value) * 448 + 256) / 512);
    }
    return (uint16_t)(512 + ((uint32_t)(value - 512) * 448 + 255) / 511);
}

static void npa_store_p010(uint8_t *destination, uint16_t value)
{
    uint16_t packed = (uint16_t)(value << 6);
    memcpy(destination, &packed, sizeof(packed));
}

int npa_pack_yuvp10_to_biplanar(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int horizontal_subsample,
    int vertical_subsample,
    int full_range
)
{
    if (!source_y || !source_u || !source_v || !destination_y || !destination_uv
        || width <= 0 || height <= 0 || width == INT_MAX || height == INT_MAX
        || (horizontal_subsample != 0 && horizontal_subsample != 1)
        || (vertical_subsample != 0 && vertical_subsample != 1)) {
        return -1;
    }

    size_t y_row_bytes = (size_t)width * 2;
    size_t chroma_width = ((size_t)width + ((1U << horizontal_subsample) - 1))
        >> horizontal_subsample;
    size_t chroma_rows = ((size_t)height + ((1U << vertical_subsample) - 1))
        >> vertical_subsample;
    size_t chroma_row_bytes = chroma_width * 2;
    size_t destination_chroma_row_bytes = chroma_width * 4;
    if (y_row_bytes > INT_MAX || chroma_row_bytes > INT_MAX
        || destination_chroma_row_bytes > INT_MAX
        || !npa_valid_plane_stride(source_y_stride, y_row_bytes)
        || !npa_valid_plane_stride(source_u_stride, chroma_row_bytes)
        || !npa_valid_plane_stride(source_v_stride, chroma_row_bytes)
        || destination_y_stride < (int)y_row_bytes
        || destination_uv_stride < (int)destination_chroma_row_bytes) {
        return -1;
    }

    for (int row = 0; row < height; row++) {
        const uint8_t *source = source_y + (ptrdiff_t)row * source_y_stride;
        uint8_t *destination = destination_y + (size_t)row * destination_y_stride;
        for (int column = 0; column < width; column++) {
            uint16_t value = npa_load_u16(source + (size_t)column * 2);
            if (full_range) {
                value = npa_full_to_video_luma(value);
            }
            npa_store_p010(destination + (size_t)column * 2, value);
        }
    }

    for (size_t row = 0; row < chroma_rows; row++) {
        const uint8_t *source_u_row = source_u + (ptrdiff_t)row * source_u_stride;
        const uint8_t *source_v_row = source_v + (ptrdiff_t)row * source_v_stride;
        uint8_t *destination = destination_uv + row * (size_t)destination_uv_stride;
        for (size_t column = 0; column < chroma_width; column++) {
            uint16_t u = npa_load_u16(source_u_row + column * 2);
            uint16_t v = npa_load_u16(source_v_row + column * 2);
            if (full_range) {
                u = npa_full_to_video_chroma(u);
                v = npa_full_to_video_chroma(v);
            }
            npa_store_p010(destination + column * 4, u);
            npa_store_p010(destination + column * 4 + 2, v);
        }
    }
    return 0;
}

int npa_pack_yuv420p10_to_p010(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int full_range
)
{
    return npa_pack_yuvp10_to_biplanar(
        source_y, source_u, source_v,
        source_y_stride, source_u_stride, source_v_stride,
        destination_y, destination_uv,
        destination_y_stride, destination_uv_stride,
        width, height, 1, 1, full_range
    );
}

static uint16_t npa_load_sample16(const uint8_t *source)
{
    uint16_t value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static uint16_t npa_full_range_luma_to_video16(uint16_t value, uint32_t maximum)
{
    return (uint16_t)(4096 + ((uint64_t)value * 56064 + maximum / 2) / maximum);
}

static uint16_t npa_full_range_chroma_to_video16(uint16_t value, uint32_t center, uint32_t maximum)
{
    if (value <= center) {
        return (uint16_t)(32768 - ((uint64_t)(center - value) * 28672 + center / 2) / center);
    }
    return (uint16_t)(32768 + ((uint64_t)(value - center) * 28672
        + (maximum - center) / 2) / (maximum - center));
}

static void npa_store_sample16(uint8_t *destination, uint16_t value)
{
    memcpy(destination, &value, sizeof(value));
}

int npa_pack_yuvp16_to_biplanar16(
    const uint8_t *source_y,
    const uint8_t *source_u,
    const uint8_t *source_v,
    int source_y_stride,
    int source_u_stride,
    int source_v_stride,
    uint8_t *destination_y,
    uint8_t *destination_uv,
    int destination_y_stride,
    int destination_uv_stride,
    int width,
    int height,
    int horizontal_subsample,
    int vertical_subsample,
    int input_bit_depth,
    int full_range
)
{
    if (!source_y || !source_u || !source_v || !destination_y || !destination_uv
        || width <= 0 || height <= 0 || width == INT_MAX || height == INT_MAX
        || (horizontal_subsample != 0 && horizontal_subsample != 1)
        || (vertical_subsample != 0 && vertical_subsample != 1)
        || (input_bit_depth != 12 && input_bit_depth != 16)) {
        return -1;
    }

    size_t y_row_bytes = (size_t)width * 2;
    size_t chroma_width = ((size_t)width + ((1U << horizontal_subsample) - 1))
        >> horizontal_subsample;
    size_t source_chroma_rows = ((size_t)height + ((1U << vertical_subsample) - 1))
        >> vertical_subsample;
    size_t destination_chroma_rows = vertical_subsample ? (size_t)height : source_chroma_rows;
    size_t chroma_row_bytes = chroma_width * 2;
    size_t destination_chroma_row_bytes = chroma_width * 4;
    if (y_row_bytes > INT_MAX || chroma_row_bytes > INT_MAX
        || destination_chroma_row_bytes > INT_MAX
        || !npa_valid_plane_stride(source_y_stride, y_row_bytes)
        || !npa_valid_plane_stride(source_u_stride, chroma_row_bytes)
        || !npa_valid_plane_stride(source_v_stride, chroma_row_bytes)
        || destination_y_stride < (int)y_row_bytes
        || destination_uv_stride < (int)destination_chroma_row_bytes) {
        return -1;
    }

    uint32_t maximum = input_bit_depth == 12 ? 4095U : 65535U;
    uint32_t center = input_bit_depth == 12 ? 2048U : 32768U;
    for (int row = 0; row < height; row++) {
        const uint8_t *source = source_y + (ptrdiff_t)row * source_y_stride;
        uint8_t *destination = destination_y + (size_t)row * destination_y_stride;
        for (int column = 0; column < width; column++) {
            uint16_t value = npa_load_sample16(source + (size_t)column * 2);
            if (input_bit_depth == 12) {
                value &= 0x0fff;
            }
            if (full_range) {
                value = npa_full_range_luma_to_video16(value, maximum);
            } else if (input_bit_depth == 12) {
                value = (uint16_t)(value << 4);
            }
            npa_store_sample16(destination + (size_t)column * 2, value);
        }
    }

    for (size_t row = 0; row < destination_chroma_rows; row++) {
        size_t source_row = vertical_subsample ? row >> 1 : row;
        if (source_row >= source_chroma_rows) {
            source_row = source_chroma_rows - 1;
        }
        const uint8_t *source_u_row = source_u + (ptrdiff_t)source_row * source_u_stride;
        const uint8_t *source_v_row = source_v + (ptrdiff_t)source_row * source_v_stride;
        uint8_t *destination = destination_uv + row * (size_t)destination_uv_stride;
        for (size_t column = 0; column < chroma_width; column++) {
            uint16_t u = npa_load_sample16(source_u_row + column * 2);
            uint16_t v = npa_load_sample16(source_v_row + column * 2);
            if (input_bit_depth == 12) {
                u &= 0x0fff;
                v &= 0x0fff;
            }
            if (full_range) {
                u = npa_full_range_chroma_to_video16(u, center, maximum);
                v = npa_full_range_chroma_to_video16(v, center, maximum);
            } else if (input_bit_depth == 12) {
                u = (uint16_t)(u << 4);
                v = (uint16_t)(v << 4);
            }
            npa_store_sample16(destination + column * 4, u);
            npa_store_sample16(destination + column * 4 + 2, v);
        }
    }
    return 0;
}
